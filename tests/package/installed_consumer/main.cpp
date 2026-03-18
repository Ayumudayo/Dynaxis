#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
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
#include "server/core/fps/direct_bind.hpp"
#include "server/core/fps/direct_delivery.hpp"
#include "server/core/fps/transport_quality.hpp"
#include "server/core/fps/transport_policy.hpp"
#include "server/core/fps/runtime.hpp"
#include "server/core/worlds/migration.hpp"
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
    auto runtime_flag = std::make_shared<int>(3);
    local_context.set(runtime_flag);
    runtime.set_service(runtime_flag);
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
    server::core::fps::WorldRuntime fps_runtime;
    const auto staged_1 = fps_runtime.stage_input(1, server::core::fps::InputCommand{.input_seq = 1, .move_x_mm = 100});
    if (!require_true(staged_1.disposition == server::core::fps::StageInputDisposition::kAccepted, "fps stage_input should accept first input")) {
        return 1;
    }
    const auto first_tick = fps_runtime.tick();
    if (!require_true(first_tick.size() == 1, "fps first tick should emit one viewer update")) {
        return 1;
    }
    if (!require_true(first_tick.front().kind == server::core::fps::ReplicationKind::kSnapshot, "fps first tick should emit snapshot")) {
        return 1;
    }

    const auto actor_id = fps_runtime.actor_id_for_session(1);
    if (!require_true(actor_id.has_value(), "fps actor should exist after first input")) {
        return 1;
    }

    const auto staged_2 = fps_runtime.stage_input(1, server::core::fps::InputCommand{.input_seq = 2, .move_x_mm = 200});
    if (!require_true(staged_2.target_server_tick == 2, "fps target tick should advance")) {
        return 1;
    }
    const auto second_tick = fps_runtime.tick();
    if (!require_true(second_tick.size() == 1, "fps second tick should emit one viewer update")) {
        return 1;
    }
    if (!require_true(second_tick.front().kind == server::core::fps::ReplicationKind::kDelta, "fps second tick should emit delta")) {
        return 1;
    }
    if (!require_true(!second_tick.front().actors.empty(), "fps delta should include actor payload")) {
        return 1;
    }

    const auto rewind = fps_runtime.rewind_at_or_before(server::core::fps::RewindQuery{
        .actor_id = *actor_id,
        .server_tick = 2,
    });
    if (!require_true(rewind.has_value(), "fps rewind should return a sample")) {
        return 1;
    }
    if (!require_true(rewind->sample.x_mm == 300, "fps rewind should return authoritative accumulated position")) {
        return 1;
    }

    const auto route = server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = true,
    });
    if (!require_true(route.route == server::core::fps::DirectDeliveryRoute::kRudp, "direct delivery should select rudp when established")) {
        return 1;
    }
    if (!require_true(route.reason == server::core::fps::DirectDeliveryReason::kRudpDirect, "direct delivery should report rudp reason")) {
        return 1;
    }

    server::core::fps::UdpSequencedMetrics udp_quality;
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

    server::core::fps::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.enabled = true;
    rollout_policy.canary_percent = 100;
    rollout_policy.opcode_allowlist = server::core::fps::parse_direct_opcode_allowlist("0x0206");
    if (!require_true(rollout_policy.opcode_allowed(0x0206), "rollout policy should accept configured opcode")) {
        return 1;
    }
    if (!require_true(rollout_policy.session_selected("consumer-session", 77), "rollout policy should select session at 100 percent canary")) {
        return 1;
    }
    const auto attach = server::core::fps::evaluate_direct_attach(rollout_policy, "consumer-session", 77);
    if (!require_true(attach.mode == server::core::fps::DirectAttachMode::kRudpCanary, "attach decision should select rudp canary")) {
        return 1;
    }
    if (!require_true(attach.reason == server::core::fps::DirectAttachReason::kCanarySelected, "attach decision should explain canary selection")) {
        return 1;
    }
    const auto bind_request_payload = server::core::fps::encode_direct_bind_request_payload(server::core::fps::DirectBindRequest{
        .session_id = "consumer-session",
        .nonce = 99,
        .expires_unix_ms = 777,
        .token = "token",
    });
    server::core::fps::DirectBindRequest bind_request{};
    if (!require_true(server::core::fps::decode_direct_bind_request_payload(bind_request_payload, bind_request), "bind request payload should decode")) {
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

    server::core::app::install_termination_signal_handlers();
    (void)server::core::app::termination_signal_received();

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
    (void)server::core::runtime_metrics::snapshot();
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
    auto service = std::make_shared<InstalledConsumerService>();
    runtime.bridge_service(service);
    const auto resolved = server::core::util::services::get<InstalledConsumerService>();
    if (!require_true(static_cast<bool>(resolved), "bridged service should be visible in global registry")) {
        return 1;
    }
    server::core::util::services::clear();

    return 0;
}
