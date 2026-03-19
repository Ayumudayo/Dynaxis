#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <string>

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
#include "server/core/worlds/kubernetes.hpp"
#include "server/core/worlds/world_drain.hpp"
#include "server/core/worlds/topology.hpp"
#include "server/core/worlds/world_transfer.hpp"
#include "server/core/compression/compressor.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/config/options.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/http_server.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/net/connection.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"
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

class StableScenarioUnitOfWork final : public server::core::storage_execution::IUnitOfWork {
public:
    void commit() override { committed_ = true; }
    void rollback() override { rolled_back_ = true; }

    bool committed() const noexcept { return committed_; }
    bool rolled_back() const noexcept { return rolled_back_; }

private:
    bool committed_{false};
    bool rolled_back_{false};
};

class StableScenarioConnectionPool final : public server::core::storage_execution::IConnectionPool {
public:
    std::unique_ptr<server::core::storage_execution::IUnitOfWork> make_unit_of_work() override {
        return std::make_unique<StableScenarioUnitOfWork>();
    }

    bool health_check() override { return true; }
};

struct StableScenarioPluginApi {
    std::uint32_t abi_version{1};
    const char* name{"stable_header_scenarios"};
};

void scenario_api_and_app() {
    (void)server::core::api::version_string();

    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("stable_header_scenarios")
            .declare_dependency("dep")
            .build();
    server::core::app::AppHost& host = runtime.host();
    runtime.set_dependency_ok("dep", true);
    runtime.mark_running();

    server::core::app::EngineContext local_context;
    auto local_value = std::make_shared<int>(7);
    local_context.set(local_value);
    runtime.set_service(local_value);

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
    (void)host.lifecycle_metrics_text();
    (void)host.dependency_metrics_text();
    (void)runtime.snapshot();
}

void scenario_concurrency_and_config() {
    server::core::SessionOptions options{};
    options.read_timeout_ms = 250;

    server::core::concurrent::TaskScheduler scheduler;
    scheduler.post([] {});
    (void)scheduler.poll();

    server::core::JobQueue queue(8);
    server::core::ThreadManager workers(queue);
    workers.Start(1);
    (void)queue.TryPush([] {});
    workers.Stop();
}

void scenario_metrics_and_runtime() {
    server::core::metrics::MetricsHttpServer metrics_server(0, [] { return std::string{}; });
    server::core::realtime::WorldRuntime fps_runtime(server::core::realtime::RuntimeConfig{
        .max_delta_actors_per_tick = 2,
    });
    (void)fps_runtime.stage_input(1, server::core::realtime::InputCommand{.input_seq = 1});
    (void)fps_runtime.tick();
    (void)server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
    });
    server::core::realtime::UdpSequencedMetrics udp_quality;
    (void)udp_quality.on_packet(10, 1000);
    (void)udp_quality.on_packet(12, 1020);
    (void)server::core::worlds::reconcile_topology(
        server::core::worlds::DesiredTopologyDocument{},
        std::vector<server::core::worlds::ObservedTopologyPool>{});
    (void)server::core::worlds::plan_topology_actuation(
        server::core::worlds::DesiredTopologyDocument{},
        std::vector<server::core::worlds::ObservedTopologyPool>{});
    (void)server::core::worlds::evaluate_topology_actuation_request_status(
        server::core::worlds::TopologyActuationRequestDocument{
            .request_id = "stable-header-request",
            .actions = {{
                .world_id = "starter-a",
                .shard = "alpha",
                .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                .replica_delta = 1,
            }},
        },
        server::core::worlds::DesiredTopologyDocument{
            .revision = 1,
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        });
    (void)server::core::worlds::evaluate_topology_actuation_execution_status(
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
            .request_id = "stable-header-request",
            .revision = 1,
            .actions = {{
                .world_id = "starter-a",
                .shard = "alpha",
                .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                .replica_delta = 1,
            }},
        },
        server::core::worlds::DesiredTopologyDocument{
            .revision = 1,
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        });
    (void)server::core::worlds::evaluate_topology_actuation_realization_status(
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
            .request_id = "stable-header-request",
            .revision = 1,
            .actions = {{
                .world_id = "starter-a",
                .shard = "alpha",
                .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                .replica_delta = 1,
            }},
        },
        server::core::worlds::DesiredTopologyDocument{
            .revision = 1,
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 2, .ready_instances = 1},
        });
    (void)server::core::worlds::evaluate_topology_actuation_adapter_status(
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
            .request_id = "stable-header-request",
            .revision = 1,
            .actions = {{
                .world_id = "starter-a",
                .shard = "alpha",
                .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                .replica_delta = 1,
            }},
        },
        server::core::worlds::DesiredTopologyDocument{
            .revision = 1,
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 2, .ready_instances = 1},
        });
    (void)server::core::worlds::find_topology_actuation_runtime_assignment(
        server::core::worlds::TopologyActuationRuntimeAssignmentDocument{
            .adapter_id = "adapter-a",
            .revision = 2,
            .lease_revision = 1,
            .assignments = {{
                .instance_id = "server-2",
                .world_id = "starter-a",
                .shard = "alpha",
                .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            }},
        },
        "server-2");
    (void)server::core::worlds::evaluate_world_transfer(
        server::core::worlds::ObservedWorldTransferState{
            .draining = true,
            .replacement_owner_instance_id = "server-2",
        });
    (void)server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{
            .draining = true,
            .instances = {{.instance_id = "server-1", .ready = true, .active_sessions = 1}},
        });
    (void)server::core::worlds::evaluate_world_drain_orchestration(
        server::core::worlds::WorldDrainStatus{
            .world_id = "starter-a",
            .phase = server::core::worlds::WorldDrainPhase::kDrained,
            .summary = {.drain_declared = true},
        },
        std::nullopt,
        std::nullopt);
    (void)server::core::worlds::evaluate_world_migration(
        server::core::worlds::ObservedWorldMigrationWorld{
            .world_id = "starter-a",
        },
        server::core::worlds::WorldMigrationEnvelope{
            .target_world_id = "starter-b",
            .target_owner_instance_id = "server-2",
        },
        std::nullopt);
    server::core::discovery::InMemoryStateBackend discovery_backend;
    (void)discovery_backend.upsert(server::core::discovery::InstanceRecord{
        .instance_id = "server-a",
        .role = "chat",
        .ready = true,
    });
    (void)server::core::discovery::select_instances(
        discovery_backend.list_instances(),
        server::core::discovery::InstanceSelector{.roles = {"chat"}});
    (void)server::core::discovery::parse_world_lifecycle_policy(
        server::core::discovery::serialize_world_lifecycle_policy(
            server::core::discovery::WorldLifecyclePolicy{.draining = true}));
    server::core::realtime::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.enabled = true;
    rollout_policy.canary_percent = 25;
    rollout_policy.opcode_allowlist = server::core::realtime::parse_direct_opcode_allowlist("0x0206,0x0208");
    (void)rollout_policy.session_selected("session-a", 101);
    (void)server::core::realtime::evaluate_direct_attach(rollout_policy, "session-a", 101);
    (void)server::core::realtime::encode_direct_bind_response_payload(
        0,
        server::core::realtime::DirectBindTicket{
            .session_id = "session-a",
            .nonce = 1,
            .expires_unix_ms = 2,
            .token = "token",
        },
        "issued");

    server::core::metrics::Counter& counter = server::core::metrics::get_counter("stable_header_counter");
    counter.inc();

    server::core::metrics::Gauge& gauge = server::core::metrics::get_gauge("stable_header_gauge");
    gauge.set(1.0);

    std::ostringstream metrics;
    server::core::metrics::append_build_info(metrics);
}

void scenario_worlds_kubernetes() {
    const auto binding = server::core::worlds::make_kubernetes_pool_binding(
        server::core::worlds::DesiredTopologyPool{
            .world_id = "starter-a",
            .shard = "alpha",
            .replicas = 2,
        },
        "dynaxis-dev");
    server::core::worlds::TopologyActuationRuntimeAssignmentDocument assignment_document{
        .assignments = {{
            .instance_id = "server-1",
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        }},
    };
    (void)server::core::worlds::count_topology_actuation_runtime_assignments(
        assignment_document,
        "starter-a",
        "alpha",
        server::core::worlds::TopologyActuationActionKind::kScaleOutPool);
    (void)server::core::worlds::evaluate_kubernetes_pool_orchestration(
        binding,
        server::core::worlds::KubernetesPoolObservation{
            .current_spec_replicas = 2,
            .ready_replicas = 2,
            .available_replicas = 2,
            .assigned_runtime_instances = 1,
            .idle_ready_runtime_instances = 1,
        },
        server::core::worlds::TopologyActuationAdapterLeaseAction{
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 1,
        },
        std::nullopt);
}

void scenario_storage_execution() {
    auto storage_pool = std::make_shared<StableScenarioConnectionPool>();
    server::core::storage_execution::DbWorkerPool storage_workers(storage_pool, 4);
    storage_workers.start(1);

    std::promise<void> storage_completion;
    auto storage_future = storage_completion.get_future();
    storage_workers.submit(
        [&](server::core::storage_execution::IUnitOfWork&) {
            storage_completion.set_value();
        },
        true);
    (void)storage_future.wait_for(std::chrono::milliseconds(200));
    storage_workers.stop();

    (void)server::core::storage_execution::retry_backoff_upper_bound_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kLinear,
            .base_delay_ms = 250,
            .max_delay_ms = 1'000,
        },
        3);

    std::mt19937_64 retry_rng(11);
    (void)server::core::storage_execution::sample_retry_backoff_delay_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kExponentialFullJitter,
            .base_delay_ms = 500,
            .max_delay_ms = 2'000,
        },
        2,
        retry_rng);
}

void scenario_extensibility() {
    server::core::plugin::SharedLibrary plugin_library;
    using StableScenarioPluginHost = server::core::plugin::PluginHost<StableScenarioPluginApi>;
    StableScenarioPluginHost::Config plugin_host_cfg{};
    plugin_host_cfg.plugin_path = std::filesystem::path{"stable_scenario_plugin"};
    plugin_host_cfg.cache_dir = std::filesystem::temp_directory_path() / "stable_scenario_plugin_cache";
    plugin_host_cfg.entrypoint_symbol = "stable_scenario_plugin_api_v1";
    plugin_host_cfg.api_resolver = [](void*, std::string&) -> const StableScenarioPluginApi* { return nullptr; };
    plugin_host_cfg.api_validator = [](const StableScenarioPluginApi*, std::string&) { return true; };
    plugin_host_cfg.instance_creator = [](const StableScenarioPluginApi*, std::string&) -> void* { return nullptr; };
    plugin_host_cfg.instance_destroyer = [](const StableScenarioPluginApi*, void*) {};
    StableScenarioPluginHost plugin_host(std::move(plugin_host_cfg));

    using StableScenarioPluginChain = server::core::plugin::PluginChainHost<StableScenarioPluginApi>;
    StableScenarioPluginChain::Config plugin_chain_cfg{};
    plugin_chain_cfg.cache_dir = std::filesystem::temp_directory_path() / "stable_scenario_plugin_chain_cache";
    StableScenarioPluginChain plugin_chain(std::move(plugin_chain_cfg));

    server::core::scripting::ScriptWatcher::Config watcher_cfg{};
    watcher_cfg.scripts_dir = std::filesystem::temp_directory_path() / "stable_scenario_scripts";
    watcher_cfg.extensions = {".lua"};
    server::core::scripting::ScriptWatcher watcher(watcher_cfg);

    const auto sandbox_policy = server::core::scripting::sandbox::default_policy();
    (void)server::core::scripting::sandbox::is_library_allowed("utf8", sandbox_policy);
    (void)server::core::scripting::sandbox::is_symbol_forbidden("dofile", sandbox_policy);

    server::core::scripting::LuaRuntime lua_runtime;
    (void)plugin_library.is_loaded();
    (void)plugin_host.metrics_snapshot();
    (void)plugin_chain.metrics_snapshot();
    (void)watcher;
    (void)lua_runtime.enabled();
    (void)lua_runtime.metrics_snapshot();
}

void scenario_protocol_and_security() {
    server::core::protocol::PacketHeader header{};
    std::array<std::uint8_t, server::core::protocol::k_header_bytes> encoded{};
    server::core::protocol::encode_header(header, encoded.data());
    server::core::protocol::decode_header(encoded.data(), header);

    (void)server::core::protocol::MSG_PING;
    (void)server::core::protocol::FLAG_COMPRESSED;
    (void)server::core::protocol::CAP_COMPRESS_SUPP;
    (void)server::core::protocol::errc::UNKNOWN_MSG_ID;

    (void)server::core::compression::Compressor::get_max_compressed_size(64);
    (void)server::core::security::Cipher::KEY_SIZE;
    (void)server::core::build_info::git_hash();
}

void scenario_net_and_utils() {
    boost::asio::io_context io;
    auto hive = std::make_shared<server::core::net::Hive>(io);
    auto connection = std::make_shared<server::core::net::Connection>(hive);
    auto runtime = server::core::app::EngineBuilder("stable_header_scenarios_net").build();
    auto peer_runtime = server::core::app::EngineBuilder("stable_header_scenarios_net_peer").build();

    server::core::Dispatcher dispatcher;
    dispatcher.register_handler(server::core::protocol::MSG_PING,
                                [](server::core::Session&, std::span<const std::uint8_t>) {});

    server::core::net::Listener listener(
        hive,
        {boost::asio::ip::address_v4::loopback(), 0},
        [connection](std::shared_ptr<server::core::net::Hive>) { return connection; });
    (void)listener.local_endpoint();

    server::core::BufferManager buffers(128, 2);
    (void)buffers.Acquire();

    server::core::log::set_level(server::core::log::level::info);
    (void)server::core::util::paths::executable_dir();

    struct StableScenarioService {
        int value{42};
    };
    struct StableScenarioPeerService {
        int value{77};
    };
    auto service = std::make_shared<StableScenarioService>();
    auto peer_service = std::make_shared<StableScenarioPeerService>();
    runtime.bridge_service(service);
    peer_runtime.bridge_service(peer_service);
    (void)runtime.snapshot();
    (void)peer_runtime.snapshot();
    runtime.clear_global_services();
    peer_runtime.clear_global_services();
}

}  // namespace

int main() {
    scenario_api_and_app();
    scenario_concurrency_and_config();
    scenario_metrics_and_runtime();
    scenario_worlds_kubernetes();
    scenario_storage_execution();
    scenario_extensibility();
    scenario_protocol_and_security();
    scenario_net_and_utils();
    return 0;
}
