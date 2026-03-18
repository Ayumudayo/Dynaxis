#include <array>
#include <chrono>
#include <cstdint>
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
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/metrics/http_server.hpp"
#include "server/core/memory/memory_pool.hpp"
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

int main() {
    (void)server::core::api::version_string();

    boost::asio::io_context io;
    server::core::net::Hive hive(io);
    auto hive_ptr = std::make_shared<server::core::net::Hive>(io);

    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("core_public_api_smoke")
            .declare_dependency("sample")
            .build();
    server::core::app::AppHost& host = runtime.host();
    runtime.set_dependency_ok("sample", true);
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

    server::core::JobQueue queue(8);
    server::core::ThreadManager workers(queue);
    workers.Start(1);
    (void)queue.TryPush([] {});
    workers.Stop();

    server::core::app::install_termination_signal_handlers();
    (void)server::core::app::termination_signal_received();
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

    server::core::SessionOptions options{};
    options.read_timeout_ms = 1000;
    server::core::fps::WorldRuntime fps_runtime(server::core::fps::RuntimeConfig{
        .max_delta_actors_per_tick = 2,
    });
    (void)fps_runtime.stage_input(1, server::core::fps::InputCommand{.input_seq = 1, .move_x_mm = 100});
    (void)fps_runtime.tick();
    const auto direct_delivery = server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
    });
    (void)direct_delivery;
    server::core::fps::UdpSequencedMetrics udp_quality;
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
    server::core::fps::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.enabled = true;
    rollout_policy.canary_percent = 100;
    rollout_policy.opcode_allowlist = server::core::fps::parse_direct_opcode_allowlist("0x0206,0x0208");
    (void)rollout_policy.session_selected("session-a", 1);
    (void)server::core::fps::evaluate_direct_attach(rollout_policy, "session-a", 1);
    const auto bind_request_payload = server::core::fps::encode_direct_bind_request_payload(server::core::fps::DirectBindRequest{
        .session_id = "session-a",
        .nonce = 7,
        .expires_unix_ms = 11,
        .token = "token",
    });
    server::core::fps::DirectBindRequest bind_request{};
    (void)server::core::fps::decode_direct_bind_request_payload(bind_request_payload, bind_request);

    server::core::metrics::MetricsHttpServer metrics_server(0, [] { return std::string{}; });

    server::core::metrics::Counter& counter = server::core::metrics::get_counter("public_api_smoke_counter");
    counter.inc();

    server::core::BufferManager buffers(256, 2);
    auto pooled = buffers.Acquire();
    (void)pooled;

    server::core::Dispatcher dispatcher;
    dispatcher.register_handler(server::core::protocol::MSG_PING,
                                [](server::core::Session&, std::span<const std::uint8_t>) {});

    server::core::protocol::PacketHeader header{};
    std::array<std::uint8_t, server::core::protocol::k_header_bytes> encoded{};
    server::core::protocol::encode_header(header, encoded.data());
    server::core::protocol::decode_header(encoded.data(), header);

    (void)server::core::protocol::FLAG_COMPRESSED;
    (void)server::core::protocol::CAP_COMPRESS_SUPP;
    (void)server::core::protocol::errc::UNKNOWN_MSG_ID;
    (void)server::core::build_info::git_hash();
    (void)server::core::runtime_metrics::snapshot();
    (void)server::core::compression::Compressor::get_max_compressed_size(32);
    (void)server::core::security::Cipher::KEY_SIZE;

    server::core::log::set_level(server::core::log::level::info);
    (void)server::core::util::paths::executable_dir();

    struct PublicSmokeService {
        int value{7};
    };
    auto service = std::make_shared<PublicSmokeService>();
    runtime.bridge_service(service);
    auto service_read = server::core::util::services::get<PublicSmokeService>();
    (void)service_read;
    server::core::util::services::clear();

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
