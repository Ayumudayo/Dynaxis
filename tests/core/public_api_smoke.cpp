#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>

#include "server/core/api/version.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/app/engine_builder.hpp"
#include "server/core/app/engine_context.hpp"
#include "server/core/app/engine_runtime.hpp"
#include "server/core/app/termination_signals.hpp"
#include "server/core/build_info.hpp"
#include "server/core/plugin/plugin_chain_host.hpp"
#include "server/core/plugin/plugin_host.hpp"
#include "server/core/plugin/shared_library.hpp"
#include "server/core/realtime/direct_bind.hpp"
#include "server/core/realtime/direct_delivery.hpp"
#include "server/core/realtime/simulation_phase.hpp"
#include "server/core/realtime/transport_quality.hpp"
#include "server/core/realtime/transport_policy.hpp"
#include "server/core/realtime/runtime.hpp"
#include "server/core/discovery/instance_registry.hpp"
#include "server/core/discovery/world_lifecycle_policy.hpp"
#include "server/core/storage_execution/connection_pool.hpp"
#include "server/core/storage_execution/db_worker_pool.hpp"
#include "server/core/storage_execution/retry_backoff.hpp"
#include "server/core/storage_execution/unit_of_work.hpp"
#include "server/core/worlds/migration.hpp"
#include "server/core/worlds/aws.hpp"
#include "server/core/worlds/kubernetes.hpp"
#include "server/core/worlds/world_drain.hpp"
#include "server/core/worlds/topology.hpp"
#include "server/core/worlds/world_transfer.hpp"
#include "server/core/compression/compressor.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/config/options.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/metrics/http_server.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/net/connection.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"
#include "server/core/net/transport_router.hpp"
#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/scripting/lua_sandbox.hpp"
#include "server/core/scripting/script_watcher.hpp"
#include "server/core/security/cipher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/paths.hpp"
#include "server/core/util/service_registry.hpp"

namespace {

class PublicSmokeUnitOfWork final : public server::core::storage_execution::IUnitOfWork {
public:
    void commit() override { committed_ = true; }
    void rollback() override { rolled_back_ = true; }

    bool committed() const noexcept { return committed_; }
    bool rolled_back() const noexcept { return rolled_back_; }

private:
    bool committed_{false};
    bool rolled_back_{false};
};

class PublicSmokeConnectionPool final : public server::core::storage_execution::IConnectionPool {
public:
    std::unique_ptr<server::core::storage_execution::IUnitOfWork> make_unit_of_work() override {
        return std::make_unique<PublicSmokeUnitOfWork>();
    }

    bool health_check() override { return true; }
};

struct PublicSmokePluginApi {
    std::uint32_t abi_version{1};
    const char* name{"public_api_smoke"};
};

class PublicSmokeTransportSession final : public server::core::net::ITransportSession {
public:
    server::core::protocol::SessionStatus session_status() const noexcept override {
        return server::core::protocol::SessionStatus::kAny;
    }

    bool post_serialized(std::function<void()> fn) override {
        fn();
        return true;
    }

    void send_error(std::uint16_t code, std::string_view message) override {
        last_error_code = code;
        last_error_message.assign(message);
    }

    std::uint16_t last_error_code{0};
    std::string last_error_message;
};

} // namespace

int main() {
    (void)server::core::api::version_string();

    boost::asio::io_context io;
    server::core::net::Hive hive(io);
    auto hive_ptr = std::make_shared<server::core::net::Hive>(io);

    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("core_public_api_smoke")
            .declare_dependency("sample")
            .register_module(
                "runtime-sample",
                [](server::core::app::EngineRuntime&) {},
                [] {},
                []() {
                    server::core::app::EngineRuntime::WatchdogStatus status;
                    status.healthy = true;
                    status.detail = "runtime-sample-ready";
                    return status;
                })
            .build();
    server::core::app::AppHost& host = runtime.host();
    runtime.set_dependency_ok("sample", true);
    runtime.start_modules();
    runtime.mark_running();

    server::core::app::EngineContext local_context;
    struct PublicSmokeContextService {
        int value{11};
    };
    auto local_context_service = std::make_shared<PublicSmokeContextService>();
    local_context.set(local_context_service);
    runtime.set_service(local_context_service);

    server::core::concurrent::TaskScheduler scheduler;
    scheduler.post([] {});
    (void)scheduler.poll();
    int controlled_total = 0;
    const auto cancel_group = scheduler.create_cancel_group();
    server::core::concurrent::TaskScheduler::TaskOptions delayed_options{};
    delayed_options.cancel_group = cancel_group;
    const auto delayed = scheduler.schedule_controlled(
        [&controlled_total] { controlled_total += 1; },
        std::chrono::milliseconds(5),
        delayed_options);
    if (!scheduler.reschedule(delayed.cancel_token, std::chrono::milliseconds(20))) {
        return 1;
    }
    if (scheduler.cancel(cancel_group) != 1u) {
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (scheduler.poll() != 0u || controlled_total != 0) {
        return 1;
    }
    std::chrono::milliseconds repeat_interval{0};
    std::size_t repeat_run_count = std::numeric_limits<std::size_t>::max();
    server::core::concurrent::TaskScheduler::RepeatPolicy repeat_policy{};
    repeat_policy.interval = std::chrono::milliseconds(1);
    const auto repeat = scheduler.schedule_every_controlled(
        [&repeat_interval, &repeat_run_count](const server::core::concurrent::TaskScheduler::RepeatContext& context) {
            repeat_interval = std::chrono::duration_cast<std::chrono::milliseconds>(context.current_interval);
            repeat_run_count = context.run_count;
            return server::core::concurrent::TaskScheduler::RepeatDecision::kStop;
        },
        repeat_policy);
    repeat_policy.interval = std::chrono::milliseconds(2);
    if (!scheduler.update_repeat_policy(repeat.cancel_token, repeat_policy)) {
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (scheduler.poll() != 1u || repeat_run_count != 0u || repeat_interval != std::chrono::milliseconds(2)) {
        return 1;
    }
    server::core::runtime_metrics::set_liveness_state(server::core::runtime_metrics::LivenessState::kRunning);
    server::core::runtime_metrics::record_watchdog_sample(
        "public-smoke-watchdog",
        true,
        "public-smoke-ready");
    server::core::runtime_metrics::record_watchdog_sample(
        "public-smoke-watchdog",
        false,
        "public-smoke-late");
    server::core::runtime_metrics::record_watchdog_sample(
        "public-smoke-watchdog",
        false,
        "public-smoke-late-again");
    server::core::runtime_metrics::record_watchdog_freeze_suspect(
        "public-smoke-watchdog",
        std::chrono::milliseconds(4),
        std::chrono::milliseconds(1),
        "public-smoke-freeze");
    server::core::runtime_metrics::record_exception_recoverable();
    const auto process_snapshot = server::core::runtime_metrics::snapshot();
    const auto watchdog_snapshot = server::core::runtime_metrics::watchdog_snapshot();
    const auto detailed_telemetry = server::core::runtime_metrics::detailed_telemetry_snapshot();
    server::core::realtime::SimulationPhaseContext phase_context{
        .server_tick = 1,
        .actor_count = 0,
        .viewer_count = 0,
        .staged_input_count = 0,
        .replication_update_count = 0,
    };
    (void)phase_context;
    if (process_snapshot.liveness_state != server::core::runtime_metrics::LivenessState::kDegraded
        || process_snapshot.watchdog_total < 1
        || process_snapshot.watchdog_freeze_suspect_total < 1
        || process_snapshot.detailed_telemetry_activation_total < 1
        || watchdog_snapshot.empty()
        || watchdog_snapshot.front().name != "public-smoke-watchdog"
        || detailed_telemetry.trigger.empty()) {
        return 1;
    }

    server::core::JobQueue queue(8);
    server::core::ThreadManager workers(queue);
    workers.Start(1);
    (void)queue.TryPush([] {});
    workers.Stop();

    runtime.start_admin_http(
        0,
        [] { return std::string{}; },
        server::core::metrics::MetricsHttpServer::LogsCallback{},
        [](const server::core::metrics::MetricsHttpServer::HttpRequest&)
            -> std::optional<server::core::metrics::MetricsHttpServer::RouteResponse> {
            return std::nullopt;
        });
    runtime.request_stop();
    runtime.wait_for_stop(std::chrono::milliseconds(1));
    runtime.run_shutdown();
    runtime.mark_stopped();
    const auto runtime_snapshot = runtime.snapshot();
    if (runtime_snapshot.registered_module_count != 1
        || runtime_snapshot.started_module_count != 0
        || runtime_snapshot.watchdog_count != 1
        || runtime_snapshot.unhealthy_watchdog_count != 0) {
        return 1;
    }
    const auto runtime_modules = runtime.module_snapshot();
    if (runtime_modules.size() != 1
        || runtime_modules.front().name != "runtime-sample"
        || runtime_modules.front().watchdog_detail != "runtime-sample-ready") {
        return 1;
    }

    server::core::SessionOptions options{};
    options.read_timeout_ms = 1000;
    server::core::realtime::WorldRuntime fps_runtime(server::core::realtime::RuntimeConfig{
        .max_delta_actors_per_tick = 2,
    });
    (void)fps_runtime.stage_input(1, server::core::realtime::InputCommand{.input_seq = 1, .move_x_mm = 100});
    (void)fps_runtime.tick();
    const auto direct_delivery = server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
    });
    (void)direct_delivery;
    server::core::realtime::UdpSequencedMetrics udp_quality;
    (void)udp_quality.on_packet(1, 1000);
    const auto topology_reconciliation = server::core::worlds::reconcile_topology(
        server::core::worlds::DesiredTopologyDocument{
            .topology_id = "starter",
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 1}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        });
    (void)topology_reconciliation;
    const auto topology_actuation = server::core::worlds::plan_topology_actuation(
        server::core::worlds::DesiredTopologyDocument{
            .topology_id = "starter",
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        });
    (void)topology_actuation;
    const auto topology_actuation_request_status =
        server::core::worlds::evaluate_topology_actuation_request_status(
            server::core::worlds::TopologyActuationRequestDocument{
                .request_id = "starter-scale",
                .basis_topology_revision = 1,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 1,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
            });
    (void)topology_actuation_request_status;
    const auto topology_actuation_execution_status =
        server::core::worlds::evaluate_topology_actuation_execution_status(
            server::core::worlds::TopologyActuationExecutionDocument{
                .executor_id = "executor-a",
                .request_revision = 1,
                .actions = {{
                    .action = {
                        .world_id = "starter-a",
                        .shard = "alpha",
                        .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                        .replica_delta = 1,
                    },
                    .observed_instances_before = 1,
                    .ready_instances_before = 1,
                    .state = server::core::worlds::TopologyActuationExecutionActionState::kClaimed,
                }},
            },
            server::core::worlds::TopologyActuationRequestDocument{
                .request_id = "starter-scale",
                .revision = 1,
                .basis_topology_revision = 1,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 1,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
            });
    (void)topology_actuation_execution_status;
    const auto topology_actuation_realization_status =
        server::core::worlds::evaluate_topology_actuation_realization_status(
            server::core::worlds::TopologyActuationExecutionDocument{
                .executor_id = "executor-a",
                .request_revision = 1,
                .actions = {{
                    .action = {
                        .world_id = "starter-a",
                        .shard = "alpha",
                        .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                        .replica_delta = 1,
                    },
                    .observed_instances_before = 1,
                    .ready_instances_before = 1,
                    .state = server::core::worlds::TopologyActuationExecutionActionState::kCompleted,
                }},
            },
            server::core::worlds::TopologyActuationRequestDocument{
                .request_id = "starter-scale",
                .revision = 1,
                .basis_topology_revision = 1,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 1,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 2, .ready_instances = 1},
            });
    (void)topology_actuation_realization_status;
    const auto topology_actuation_adapter_status =
        server::core::worlds::evaluate_topology_actuation_adapter_status(
            server::core::worlds::TopologyActuationAdapterLeaseDocument{
                .adapter_id = "adapter-a",
                .execution_revision = 1,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::TopologyActuationExecutionDocument{
                .executor_id = "executor-a",
                .revision = 1,
                .request_revision = 1,
                .actions = {{
                    .action = {
                        .world_id = "starter-a",
                        .shard = "alpha",
                        .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                        .replica_delta = 1,
                    },
                    .observed_instances_before = 1,
                    .ready_instances_before = 1,
                    .state = server::core::worlds::TopologyActuationExecutionActionState::kCompleted,
                }},
            },
            server::core::worlds::TopologyActuationRequestDocument{
                .request_id = "starter-scale",
                .revision = 1,
                .basis_topology_revision = 1,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 1,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 2, .ready_instances = 1},
            });
    (void)topology_actuation_adapter_status;
    server::core::worlds::TopologyActuationRuntimeAssignmentDocument topology_runtime_assignment{
        .adapter_id = "adapter-a",
        .revision = 1,
        .lease_revision = 1,
        .assignments = {{
            .instance_id = "server-2",
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        }},
    };
    (void)server::core::worlds::find_topology_actuation_runtime_assignment(
        topology_runtime_assignment,
        "server-2");
    const auto kubernetes_binding = server::core::worlds::make_kubernetes_pool_binding(
        server::core::worlds::DesiredTopologyPool{
            .world_id = "starter-a",
            .shard = "alpha",
            .replicas = 2,
        },
        "dynaxis-dev");
    const auto kubernetes_assignment_count =
        server::core::worlds::count_topology_actuation_runtime_assignments(
            topology_runtime_assignment,
            "starter-a",
            "alpha",
            server::core::worlds::TopologyActuationActionKind::kScaleOutPool);
    if (kubernetes_assignment_count != 1) {
        return 1;
    }
    const auto kubernetes_status = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        kubernetes_binding,
        server::core::worlds::KubernetesPoolObservation{
            .current_spec_replicas = 2,
            .ready_replicas = 2,
            .available_replicas = 2,
            .assigned_runtime_instances = kubernetes_assignment_count,
            .idle_ready_runtime_instances = 1,
        },
        server::core::worlds::TopologyActuationAdapterLeaseAction{
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 1,
        });
    if (kubernetes_status.phase != server::core::worlds::KubernetesPoolOrchestrationPhase::kComplete) {
        return 1;
    }
    const auto aws_binding = server::core::worlds::make_aws_pool_binding(
        kubernetes_binding,
        server::core::worlds::AwsAdapterDefaults{
            .cluster_name = "eks-dev",
            .placement = {
                .region = "ap-northeast-2",
                .availability_zones = {"ap-northeast-2a", "ap-northeast-2c"},
                .subnet_ids = {"subnet-a", "subnet-c"},
            },
            .listener_port = 7000,
        });
    const auto aws_status = server::core::worlds::evaluate_aws_pool_adapter_status(
        aws_binding,
        server::core::worlds::AwsLoadBalancerObservation{
            .load_balancer_attached = true,
            .target_group_attached = true,
            .targets_healthy = true,
        },
        server::core::worlds::AwsManagedDependencyObservation{
            .redis_ready = true,
            .postgres_ready = true,
        },
        server::core::worlds::TopologyActuationAdapterLeaseAction{
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 1,
        },
        topology_runtime_assignment);
    if (aws_status.phase != server::core::worlds::AwsPoolAdapterPhase::kComplete) {
        return 1;
    }
    if (aws_binding.identity.cluster_name != "eks-dev") {
        return 1;
    }
    const auto world_transfer = server::core::worlds::evaluate_world_transfer(
        server::core::worlds::ObservedWorldTransferState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .replacement_owner_instance_id = "server-2",
            .instances = {
                {.instance_id = "server-1", .ready = true},
                {.instance_id = "server-2", .ready = true},
            },
        });
    (void)world_transfer;
    const auto world_drain = server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .instances = {
                {.instance_id = "server-1", .ready = true, .active_sessions = 1},
            },
        });
    (void)world_drain;
    const auto world_drain_orchestration = server::core::worlds::evaluate_world_drain_orchestration(
        world_drain,
        std::nullopt,
        std::nullopt);
    (void)world_drain_orchestration;
    const auto kubernetes_scale_in = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        server::core::worlds::make_kubernetes_pool_binding(
            server::core::worlds::DesiredTopologyPool{
                .world_id = "starter-a",
                .shard = "alpha",
                .replicas = 0,
            },
            "dynaxis-dev"),
        server::core::worlds::KubernetesPoolObservation{
            .current_spec_replicas = 1,
            .ready_replicas = 1,
            .available_replicas = 1,
        },
        server::core::worlds::TopologyActuationAdapterLeaseAction{
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleInPool,
            .replica_delta = 1,
        },
        world_drain_orchestration);
    (void)kubernetes_scale_in;
    const auto world_migration = server::core::worlds::evaluate_world_migration(
        server::core::worlds::ObservedWorldMigrationWorld{
            .world_id = "starter-a",
            .current_owner_instance_id = "server-1",
            .draining = true,
            .instances = {{.instance_id = "server-1", .ready = true}},
        },
        server::core::worlds::WorldMigrationEnvelope{
            .target_world_id = "starter-b",
            .target_owner_instance_id = "server-2",
            .preserve_room = true,
        },
        server::core::worlds::ObservedWorldMigrationWorld{
            .world_id = "starter-b",
            .current_owner_instance_id = "server-2",
            .instances = {{.instance_id = "server-2", .ready = true}},
        });
    (void)world_migration;
    server::core::discovery::InMemoryStateBackend discovery_backend;
    (void)discovery_backend.upsert(server::core::discovery::InstanceRecord{
        .instance_id = "server-a",
        .host = "127.0.0.1",
        .port = 7000,
        .role = "chat",
        .ready = true,
    });
    const auto discovery_selected = server::core::discovery::select_instances(
        discovery_backend.list_instances(),
        server::core::discovery::InstanceSelector{.all = true});
    (void)discovery_selected;
    const auto discovery_policy_payload = server::core::discovery::serialize_world_lifecycle_policy(
        server::core::discovery::WorldLifecyclePolicy{
            .draining = true,
            .replacement_owner_instance_id = "server-b",
        });
    (void)server::core::discovery::parse_world_lifecycle_policy(discovery_policy_payload);
    auto storage_pool = std::make_shared<PublicSmokeConnectionPool>();
    if (!storage_pool->health_check()) {
        return 1;
    }
    server::core::storage_execution::DbWorkerPool storage_workers(storage_pool, 8);
    storage_workers.start(1);
    std::promise<void> storage_completion;
    auto storage_future = storage_completion.get_future();
    storage_workers.submit(
        [&](server::core::storage_execution::IUnitOfWork&) {
            storage_completion.set_value();
        },
        true);
    if (storage_future.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready) {
        storage_workers.stop();
        return 1;
    }
    storage_workers.stop();
    const auto linear_retry_delay = server::core::storage_execution::retry_backoff_upper_bound_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kLinear,
            .base_delay_ms = 250,
            .max_delay_ms = 1'000,
        },
        3);
    if (linear_retry_delay != 750) {
        return 1;
    }
    std::mt19937_64 retry_rng(7);
    const auto jitter_retry_delay = server::core::storage_execution::sample_retry_backoff_delay_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kExponentialFullJitter,
            .base_delay_ms = 500,
            .max_delay_ms = 2'000,
        },
        2,
        retry_rng);
    if (jitter_retry_delay > 2'000) {
        return 1;
    }
    server::core::plugin::SharedLibrary plugin_library;
    (void)plugin_library.is_loaded();
    using PublicSmokePluginHost = server::core::plugin::PluginHost<PublicSmokePluginApi>;
    PublicSmokePluginHost::Config plugin_host_cfg{};
    plugin_host_cfg.plugin_path = std::filesystem::path{"public_api_smoke_plugin"};
    plugin_host_cfg.cache_dir = std::filesystem::temp_directory_path() / "public_api_smoke_plugin_cache";
    plugin_host_cfg.entrypoint_symbol = "public_api_smoke_plugin_api_v1";
    plugin_host_cfg.api_resolver = [](void*, std::string&) -> const PublicSmokePluginApi* { return nullptr; };
    plugin_host_cfg.api_validator = [](const PublicSmokePluginApi*, std::string&) { return true; };
    plugin_host_cfg.instance_creator = [](const PublicSmokePluginApi*, std::string&) -> void* { return nullptr; };
    plugin_host_cfg.instance_destroyer = [](const PublicSmokePluginApi*, void*) {};
    PublicSmokePluginHost plugin_host(std::move(plugin_host_cfg));
    (void)plugin_host.metrics_snapshot();
    using PublicSmokePluginChain = server::core::plugin::PluginChainHost<PublicSmokePluginApi>;
    PublicSmokePluginChain::Config plugin_chain_cfg{};
    plugin_chain_cfg.plugins_dir = std::filesystem::temp_directory_path() / "public_api_smoke_plugins";
    plugin_chain_cfg.cache_dir = std::filesystem::temp_directory_path() / "public_api_smoke_chain_cache";
    plugin_chain_cfg.entrypoint_symbol = "public_api_smoke_plugin_api_v1";
    plugin_chain_cfg.api_resolver = [](void*, std::string&) -> const PublicSmokePluginApi* { return nullptr; };
    plugin_chain_cfg.api_validator = [](const PublicSmokePluginApi*, std::string&) { return true; };
    plugin_chain_cfg.instance_creator = [](const PublicSmokePluginApi*, std::string&) -> void* { return nullptr; };
    plugin_chain_cfg.instance_destroyer = [](const PublicSmokePluginApi*, void*) {};
    PublicSmokePluginChain plugin_chain(std::move(plugin_chain_cfg));
    (void)plugin_chain.metrics_snapshot();
    server::core::scripting::ScriptWatcher::Config watcher_cfg{};
    watcher_cfg.scripts_dir = std::filesystem::temp_directory_path() / "public_api_smoke_scripts";
    watcher_cfg.extensions = {".lua"};
    server::core::scripting::ScriptWatcher watcher(watcher_cfg);
    (void)watcher;
    const auto sandbox_policy = server::core::scripting::sandbox::default_policy();
    (void)server::core::scripting::sandbox::is_library_allowed("string", sandbox_policy);
    (void)server::core::scripting::sandbox::is_symbol_forbidden("require", sandbox_policy);
    server::core::scripting::LuaRuntime lua_runtime;
    (void)lua_runtime.enabled();
    (void)lua_runtime.metrics_snapshot();
    server::core::realtime::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.enabled = true;
    rollout_policy.canary_percent = 100;
    rollout_policy.opcode_allowlist = server::core::realtime::parse_direct_opcode_allowlist("0x0206,0x0208");
    (void)rollout_policy.session_selected("session-a", 1);
    (void)server::core::realtime::evaluate_direct_attach(rollout_policy, "session-a", 1);
    const auto bind_request_payload = server::core::realtime::encode_direct_bind_request_payload(server::core::realtime::DirectBindRequest{
        .session_id = "session-a",
        .nonce = 7,
        .expires_unix_ms = 11,
        .token = "token",
    });
    server::core::realtime::DirectBindRequest bind_request{};
    (void)server::core::realtime::decode_direct_bind_request_payload(bind_request_payload, bind_request);

    server::core::metrics::MetricsHttpServer metrics_server(0, [] { return std::string{}; });

    server::core::metrics::Counter& counter = server::core::metrics::get_counter("public_api_smoke_counter");
    counter.inc();

    server::core::BufferManager buffers(256, 2);
    auto pooled = buffers.Acquire();
    (void)pooled;

    server::core::net::TransportRouter transport_router;
    auto transport_session = std::make_shared<PublicSmokeTransportSession>();
    bool transport_handler_called = false;
    transport_router.register_handler(
        server::core::protocol::MSG_PING,
        [&transport_handler_called](server::core::net::ITransportSession&,
                                    std::span<const std::uint8_t> payload) {
            transport_handler_called = payload.size() == 2;
        });
    if (!transport_router.dispatch(
            server::core::protocol::MSG_PING,
            transport_session,
            std::array<std::uint8_t, 2>{0x01, 0x02})) {
        return 1;
    }
    if (!transport_handler_called
        || transport_session->last_error_code != 0
        || !transport_session->last_error_message.empty()) {
        return 1;
    }

    server::core::protocol::PacketHeader header{};
    std::array<std::uint8_t, server::core::protocol::k_header_bytes> encoded{};
    server::core::protocol::encode_header(header, encoded.data());
    server::core::protocol::decode_header(encoded.data(), header);

    (void)server::core::protocol::FLAG_COMPRESSED;
    (void)server::core::protocol::CAP_COMPRESS_SUPP;
    (void)server::core::protocol::errc::UNKNOWN_MSG_ID;
    (void)server::core::build_info::git_hash();
    (void)server::core::compression::Compressor::get_max_compressed_size(32);
    (void)server::core::security::Cipher::KEY_SIZE;

    server::core::log::set_level(server::core::log::level::info);
    (void)server::core::util::paths::executable_dir();

    struct PublicSmokeService {
        int value{7};
    };
    struct PublicSmokePeerService {
        int value{9};
    };
    auto left_runtime = server::core::app::EngineBuilder("core_public_api_smoke_left").build();
    auto right_runtime = server::core::app::EngineBuilder("core_public_api_smoke_right").build();
    left_runtime.mark_running();
    right_runtime.mark_running();
    left_runtime.bridge_service(std::make_shared<PublicSmokeService>());
    right_runtime.bridge_service(std::make_shared<PublicSmokePeerService>());
    const auto left_snapshot = left_runtime.snapshot();
    const auto right_snapshot = right_runtime.snapshot();
    (void)left_snapshot;
    (void)right_snapshot;
    left_runtime.clear_global_services();
    right_runtime.clear_global_services();
    left_runtime.mark_stopped();
    right_runtime.mark_stopped();

    std::ostringstream metrics;
    server::core::metrics::append_build_info(metrics);

    auto connection = std::make_shared<server::core::net::Connection>(hive_ptr);
    server::core::net::Listener listener(
        hive_ptr,
        {boost::asio::ip::address_v4::loopback(), 0},
        [connection](std::shared_ptr<server::core::net::Hive>) { return connection; });
    (void)listener.local_endpoint();

    (void)host.dependency_metrics_text();
    (void)host.lifecycle_metrics_text();

    return 0;
}
