#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
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
#include "server/core/security/cipher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/paths.hpp"
#include "server/core/util/service_registry.hpp"

namespace {

bool require_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

class InstalledConsumerUnitOfWork final : public server::core::storage_execution::IUnitOfWork {
public:
    void commit() override { committed_ = true; }
    void rollback() override { rolled_back_ = true; }

    bool committed() const noexcept { return committed_; }
    bool rolled_back() const noexcept { return rolled_back_; }

private:
    bool committed_{false};
    bool rolled_back_{false};
};

class InstalledConsumerConnectionPool final : public server::core::storage_execution::IConnectionPool {
public:
    std::unique_ptr<server::core::storage_execution::IUnitOfWork> make_unit_of_work() override {
        return std::make_unique<InstalledConsumerUnitOfWork>();
    }

    bool health_check() override { return true; }
};

} // namespace

int main() {
    (void)server::core::api::version_string();

    boost::asio::io_context io;
    auto hive = std::make_shared<server::core::net::Hive>(io);

    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("installed_consumer")
            .declare_dependency("dep")
            .build();
    server::core::app::AppHost& host = runtime.host();
    runtime.set_dependency_ok("dep", true);
    runtime.mark_running();

    server::core::app::EngineContext local_context;
    server::core::app::EngineRuntime peer_runtime =
        server::core::app::EngineBuilder("installed_consumer_peer")
            .declare_dependency("dep")
            .build();
    auto runtime_flag = std::make_shared<int>(3);
    local_context.set(runtime_flag);
    runtime.set_service(runtime_flag);
    peer_runtime.set_dependency_ok("dep", true);
    peer_runtime.mark_running();
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
    server::core::realtime::WorldRuntime fps_runtime;
    const auto staged_1 = fps_runtime.stage_input(1, server::core::realtime::InputCommand{.input_seq = 1, .move_x_mm = 100});
    if (!require_true(staged_1.disposition == server::core::realtime::StageInputDisposition::kAccepted, "fps stage_input should accept first input")) {
        return 1;
    }
    const auto first_tick = fps_runtime.tick();
    if (!require_true(first_tick.size() == 1, "fps first tick should emit one viewer update")) {
        return 1;
    }
    if (!require_true(first_tick.front().kind == server::core::realtime::ReplicationKind::kSnapshot, "fps first tick should emit snapshot")) {
        return 1;
    }

    const auto actor_id = fps_runtime.actor_id_for_session(1);
    if (!require_true(actor_id.has_value(), "fps actor should exist after first input")) {
        return 1;
    }

    const auto staged_2 = fps_runtime.stage_input(1, server::core::realtime::InputCommand{.input_seq = 2, .move_x_mm = 200});
    if (!require_true(staged_2.target_server_tick == 2, "fps target tick should advance")) {
        return 1;
    }
    const auto second_tick = fps_runtime.tick();
    if (!require_true(second_tick.size() == 1, "fps second tick should emit one viewer update")) {
        return 1;
    }
    if (!require_true(second_tick.front().kind == server::core::realtime::ReplicationKind::kDelta, "fps second tick should emit delta")) {
        return 1;
    }
    if (!require_true(!second_tick.front().actors.empty(), "fps delta should include actor payload")) {
        return 1;
    }

    const auto rewind = fps_runtime.rewind_at_or_before(server::core::realtime::RewindQuery{
        .actor_id = *actor_id,
        .server_tick = 2,
    });
    if (!require_true(rewind.has_value(), "fps rewind should return a sample")) {
        return 1;
    }
    if (!require_true(rewind->sample.x_mm == 300, "fps rewind should return authoritative accumulated position")) {
        return 1;
    }

    const auto route = server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = true,
    });
    if (!require_true(route.route == server::core::realtime::DirectDeliveryRoute::kRudp, "direct delivery should select rudp when established")) {
        return 1;
    }
    if (!require_true(route.reason == server::core::realtime::DirectDeliveryReason::kRudpDirect, "direct delivery should report rudp reason")) {
        return 1;
    }

    server::core::realtime::UdpSequencedMetrics udp_quality;
    (void)udp_quality.on_packet(100, 1000);
    const auto udp_quality_update = udp_quality.on_packet(102, 1015);
    if (!require_true(udp_quality_update.accepted, "udp quality update should accept forward progress")) {
        return 1;
    }
    if (!require_true(udp_quality_update.estimated_lost_packets == 1, "udp quality update should estimate gap loss")) {
        return 1;
    }

    const auto topology_reconciliation = server::core::worlds::reconcile_topology(
        server::core::worlds::DesiredTopologyDocument{
            .topology_id = "starter",
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 1}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        });
    if (!require_true(topology_reconciliation.summary.aligned_pools == 1, "topology reconciliation should report aligned pool")) {
        return 1;
    }
    const auto topology_actuation = server::core::worlds::plan_topology_actuation(
        server::core::worlds::DesiredTopologyDocument{
            .topology_id = "starter",
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
        },
        std::vector<server::core::worlds::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        });
    if (!require_true(topology_actuation.summary.scale_out_actions == 1, "topology actuation should report scale-out action")) {
        return 1;
    }
    const auto topology_actuation_request_status =
        server::core::worlds::evaluate_topology_actuation_request_status(
            server::core::worlds::TopologyActuationRequestDocument{
                .request_id = "consumer-actuation-request",
                .basis_topology_revision = 7,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 7,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
            });
    if (!require_true(
            topology_actuation_request_status.summary.pending_actions == 1,
            "topology actuation request status should report pending action")) {
        return 1;
    }
    const auto topology_actuation_execution_status =
        server::core::worlds::evaluate_topology_actuation_execution_status(
            server::core::worlds::TopologyActuationExecutionDocument{
                .executor_id = "consumer-executor",
                .request_revision = 7,
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
                .request_id = "consumer-actuation-request",
                .revision = 7,
                .basis_topology_revision = 7,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 7,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
            });
    if (!require_true(
            topology_actuation_execution_status.summary.claimed_actions == 1,
            "topology actuation execution status should report claimed action")) {
        return 1;
    }
    const auto topology_actuation_realization_status =
        server::core::worlds::evaluate_topology_actuation_realization_status(
            server::core::worlds::TopologyActuationExecutionDocument{
                .executor_id = "consumer-executor",
                .request_revision = 7,
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
                .request_id = "consumer-actuation-request",
                .revision = 7,
                .basis_topology_revision = 7,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 7,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 2, .ready_instances = 1},
            });
    if (!require_true(
            topology_actuation_realization_status.summary.realized_actions == 1,
            "topology actuation realization status should report realized action")) {
        return 1;
    }
    const auto topology_actuation_adapter_status =
        server::core::worlds::evaluate_topology_actuation_adapter_status(
            server::core::worlds::TopologyActuationAdapterLeaseDocument{
                .adapter_id = "consumer-adapter",
                .execution_revision = 7,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::TopologyActuationExecutionDocument{
                .executor_id = "consumer-executor",
                .revision = 7,
                .request_revision = 7,
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
                .request_id = "consumer-actuation-request",
                .revision = 7,
                .basis_topology_revision = 7,
                .actions = {{
                    .world_id = "starter-a",
                    .shard = "alpha",
                    .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
                    .replica_delta = 1,
                }},
            },
            server::core::worlds::DesiredTopologyDocument{
                .topology_id = "starter",
                .revision = 7,
                .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 2}},
            },
            std::vector<server::core::worlds::ObservedTopologyPool>{
                {.world_id = "starter-a", .shard = "alpha", .instances = 2, .ready_instances = 1},
            });
    if (!require_true(
            topology_actuation_adapter_status.summary.realized_actions == 1,
            "topology actuation adapter status should report realized action")) {
        return 1;
    }
    server::core::worlds::TopologyActuationRuntimeAssignmentDocument topology_runtime_assignment{
        .adapter_id = "consumer-adapter",
        .revision = 3,
        .lease_revision = 7,
        .assignments = {{
            .instance_id = "server-2",
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        }},
    };
    const auto* runtime_assignment =
        server::core::worlds::find_topology_actuation_runtime_assignment(
            topology_runtime_assignment,
            "server-2");
    if (!require_true(runtime_assignment != nullptr, "runtime assignment should be found by instance id")) {
        return 1;
    }
    if (!require_true(runtime_assignment->world_id == "starter-a", "runtime assignment world_id mismatch")) {
        return 1;
    }
    const auto kubernetes_binding = server::core::worlds::make_kubernetes_pool_binding(
        server::core::worlds::DesiredTopologyPool{
            .world_id = "starter-a",
            .shard = "alpha",
            .replicas = 2,
        },
        "dynaxis-dev");
    if (!require_true(
            server::core::worlds::count_topology_actuation_runtime_assignments(
                topology_runtime_assignment,
                "starter-a",
                "alpha",
                server::core::worlds::TopologyActuationActionKind::kScaleOutPool) == 1,
            "kubernetes assignment count should match the runtime-assignment document")) {
        return 1;
    }
    const auto kubernetes_scale_out = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        kubernetes_binding,
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
        });
    if (!require_true(
            kubernetes_scale_out.phase == server::core::worlds::KubernetesPoolOrchestrationPhase::kComplete,
            "kubernetes scale-out contract should report complete once workload and assignment are aligned")) {
        return 1;
    }
    const auto aws_binding = server::core::worlds::make_aws_pool_binding(
        kubernetes_binding,
        server::core::worlds::AwsAdapterDefaults{
            .cluster_name = "eks-consumer",
            .placement = {
                .region = "ap-northeast-2",
                .availability_zones = {"ap-northeast-2a", "ap-northeast-2c"},
                .subnet_ids = {"subnet-a", "subnet-c"},
            },
            .listener_port = 7000,
            .redis_prefix = "consumer-redis",
            .postgres_prefix = "consumer-pg",
        });
    if (!require_true(aws_binding.identity.cluster_name == "eks-consumer", "aws binding cluster identity mismatch")) {
        return 1;
    }
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
    if (!require_true(
            aws_status.phase == server::core::worlds::AwsPoolAdapterPhase::kComplete,
            "aws adapter status should report complete once load balancer, managed dependencies, and runtime assignments are aligned")) {
        return 1;
    }
    const auto world_transfer = server::core::worlds::evaluate_world_transfer(
        server::core::worlds::ObservedWorldTransferState{
            .world_id = "starter-a",
            .owner_instance_id = "server-2",
            .draining = true,
            .replacement_owner_instance_id = "server-2",
            .instances = {
                {.instance_id = "server-1", .ready = true},
                {.instance_id = "server-2", .ready = true},
            },
        });
    if (!require_true(
            world_transfer.phase == server::core::worlds::WorldTransferPhase::kOwnerHandoffCommitted,
            "world transfer should report committed handoff when owner matches target")) {
        return 1;
    }
    const auto world_drain = server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .instances = {
                {.instance_id = "server-1", .ready = true, .active_sessions = 0},
            },
        });
    if (!require_true(
            world_drain.phase == server::core::worlds::WorldDrainPhase::kDrained,
            "world drain should report drained when no sessions remain")) {
        return 1;
    }
    const auto world_drain_orchestration = server::core::worlds::evaluate_world_drain_orchestration(
        world_drain,
        server::core::worlds::WorldTransferStatus{
            .world_id = "starter-a",
            .owner_instance_id = "server-2",
            .target_owner_instance_id = "server-2",
            .phase = server::core::worlds::WorldTransferPhase::kOwnerHandoffCommitted,
            .summary = {.transfer_declared = true, .owner_matches_target = true},
        },
        std::nullopt);
    if (!require_true(
            world_drain_orchestration.phase == server::core::worlds::WorldDrainOrchestrationPhase::kReadyToClear,
            "world drain orchestration should report ready_to_clear after committed transfer")) {
        return 1;
    }
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
    if (!require_true(
            kubernetes_scale_in.phase == server::core::worlds::KubernetesPoolOrchestrationPhase::kRetireWorkload,
            "kubernetes scale-in contract should report retire_workload after drain closure")) {
        return 1;
    }
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
    if (!require_true(
            world_migration.phase == server::core::worlds::WorldMigrationPhase::kReadyToResume,
            "world migration should report ready_to_resume when target world is ready")) {
        return 1;
    }
    server::core::discovery::InMemoryStateBackend discovery_backend;
    if (!require_true(
            discovery_backend.upsert(server::core::discovery::InstanceRecord{
                .instance_id = "server-1",
                .host = "127.0.0.1",
                .port = 7000,
                .role = "chat",
                .game_mode = "default",
                .region = "ap-northeast",
                .shard = "alpha",
                .ready = true,
            }),
            "discovery backend should accept instance upsert")) {
        return 1;
    }
    const auto discovery_selected = server::core::discovery::select_instances(
        discovery_backend.list_instances(),
        server::core::discovery::InstanceSelector{
            .roles = {"chat"},
            .regions = {"ap-northeast"},
        });
    if (!require_true(discovery_selected.size() == 1, "discovery selector should find matching instance")) {
        return 1;
    }
    const auto lifecycle_policy_payload = server::core::discovery::serialize_world_lifecycle_policy(
        server::core::discovery::WorldLifecyclePolicy{
            .draining = true,
            .replacement_owner_instance_id = "server-2",
        });
    const auto lifecycle_policy = server::core::discovery::parse_world_lifecycle_policy(lifecycle_policy_payload);
    if (!require_true(
            lifecycle_policy.has_value() && lifecycle_policy->replacement_owner_instance_id == "server-2",
            "world lifecycle policy should round-trip through discovery surface")) {
        return 1;
    }
    auto storage_pool = std::make_shared<InstalledConsumerConnectionPool>();
    if (!require_true(storage_pool->health_check(), "storage execution pool health check should succeed")) {
        return 1;
    }
    server::core::storage_execution::DbWorkerPool storage_workers(storage_pool, 4);
    storage_workers.start(1);
    std::promise<void> storage_completion;
    auto storage_future = storage_completion.get_future();
    storage_workers.submit(
        [&](server::core::storage_execution::IUnitOfWork&) {
            storage_completion.set_value();
        },
        true);
    if (!require_true(
            storage_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready,
            "storage execution worker should process one fake job")) {
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
    if (!require_true(linear_retry_delay == 750, "linear retry backoff should scale by attempt")) {
        return 1;
    }
    std::mt19937_64 retry_rng(9);
    const auto jitter_retry_delay = server::core::storage_execution::sample_retry_backoff_delay_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kExponentialFullJitter,
            .base_delay_ms = 500,
            .max_delay_ms = 2'000,
        },
        2,
        retry_rng);
    if (!require_true(jitter_retry_delay <= 2'000, "jitter retry backoff should stay within the cap")) {
        return 1;
    }

    server::core::realtime::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.enabled = true;
    rollout_policy.canary_percent = 100;
    rollout_policy.opcode_allowlist = server::core::realtime::parse_direct_opcode_allowlist("0x0206");
    if (!require_true(rollout_policy.opcode_allowed(0x0206), "rollout policy should accept configured opcode")) {
        return 1;
    }
    if (!require_true(rollout_policy.session_selected("consumer-session", 77), "rollout policy should select session at 100 percent canary")) {
        return 1;
    }
    const auto attach = server::core::realtime::evaluate_direct_attach(rollout_policy, "consumer-session", 77);
    if (!require_true(attach.mode == server::core::realtime::DirectAttachMode::kRudpCanary, "attach decision should select rudp canary")) {
        return 1;
    }
    if (!require_true(attach.reason == server::core::realtime::DirectAttachReason::kCanarySelected, "attach decision should explain canary selection")) {
        return 1;
    }
    const auto bind_request_payload = server::core::realtime::encode_direct_bind_request_payload(server::core::realtime::DirectBindRequest{
        .session_id = "consumer-session",
        .nonce = 99,
        .expires_unix_ms = 777,
        .token = "token",
    });
    server::core::realtime::DirectBindRequest bind_request{};
    if (!require_true(server::core::realtime::decode_direct_bind_request_payload(bind_request_payload, bind_request), "bind request payload should decode")) {
        return 1;
    }
    if (!require_true(bind_request.session_id == "consumer-session", "bind request should preserve session id")) {
        return 1;
    }

    server::core::concurrent::TaskScheduler scheduler;
    scheduler.post([] {});
    (void)scheduler.poll();

    server::core::SessionOptions options{};
    options.read_timeout_ms = 1000;

    server::core::JobQueue queue(4);
    server::core::ThreadManager workers(queue);
    workers.Start(1);
    workers.Stop();

    server::core::metrics::MetricsHttpServer metrics_server(0, [] { return std::string{}; });
    server::core::metrics::get_counter("installed_consumer_counter").inc();

    server::core::BufferManager buffers(128, 2);
    (void)buffers.Acquire();

    server::core::Dispatcher dispatcher;
    dispatcher.register_handler(server::core::protocol::MSG_PING,
                                [](server::core::Session&, std::span<const std::uint8_t>) {});

    server::core::protocol::PacketHeader header{};
    std::array<std::uint8_t, server::core::protocol::k_header_bytes> encoded{};
    server::core::protocol::encode_header(header, encoded.data());
    server::core::protocol::decode_header(encoded.data(), header);

    (void)server::core::build_info::git_hash();
    (void)server::core::compression::Compressor::get_max_compressed_size(32);
    (void)server::core::security::Cipher::KEY_SIZE;
    (void)server::core::protocol::FLAG_COMPRESSED;
    (void)server::core::protocol::CAP_COMPRESS_SUPP;
    (void)server::core::protocol::errc::UNKNOWN_MSG_ID;

    server::core::log::set_level(server::core::log::level::info);
    (void)server::core::util::paths::executable_dir();

    std::ostringstream metrics;
    server::core::metrics::append_build_info(metrics);

    auto connection = std::make_shared<server::core::net::Connection>(hive);
    server::core::net::Listener listener(
        hive,
        {boost::asio::ip::address_v4::loopback(), 0},
        [connection](std::shared_ptr<server::core::net::Hive>) { return connection; });
    (void)listener.local_endpoint();

    struct InstalledConsumerService {
        int value{3};
    };
    struct InstalledConsumerPeerService {
        int value{4};
    };
    auto service = std::make_shared<InstalledConsumerService>();
    auto peer_service = std::make_shared<InstalledConsumerPeerService>();
    runtime.bridge_service(service);
    peer_runtime.bridge_service(peer_service);
    const auto runtime_snapshot = runtime.snapshot();
    const auto peer_snapshot = peer_runtime.snapshot();
    if (!require_true(runtime_snapshot.context_service_count == 2, "runtime snapshot should report local context services")) {
        return 1;
    }
    if (!require_true(runtime_snapshot.compatibility_bridge_count == 1, "runtime snapshot should report one compatibility bridge")) {
        return 1;
    }
    if (!require_true(peer_snapshot.context_service_count == 1, "peer runtime snapshot should stay isolated")) {
        return 1;
    }
    if (!require_true(peer_snapshot.compatibility_bridge_count == 1, "peer runtime snapshot should report one compatibility bridge")) {
        return 1;
    }
    runtime.clear_global_services();
    const auto runtime_after_clear = runtime.snapshot();
    const auto peer_after_clear = peer_runtime.snapshot();
    if (!require_true(runtime_after_clear.compatibility_bridge_count == 0, "runtime clear_global_services should clear only its own bridges")) {
        return 1;
    }
    if (!require_true(peer_after_clear.compatibility_bridge_count == 1, "runtime clear_global_services should not clear peer bridges")) {
        return 1;
    }
    peer_runtime.clear_global_services();
    peer_runtime.request_stop();
    peer_runtime.wait_for_stop(std::chrono::milliseconds(1));
    peer_runtime.run_shutdown();
    peer_runtime.mark_stopped();

    return 0;
}
