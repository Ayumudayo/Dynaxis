#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>

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
#include "server/core/mmorpg/migration.hpp"
#include "server/core/mmorpg/topology.hpp"
#include "server/core/mmorpg/world_drain.hpp"
#include "server/core/mmorpg/world_transfer.hpp"
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
    static_assert(std::is_default_constructible_v<server::core::SessionOptions>);

    boost::asio::io_context io;
    server::core::net::Hive hive(io);
    auto hive_ptr = std::make_shared<server::core::net::Hive>(io);

    server::core::concurrent::TaskScheduler scheduler;
    scheduler.post([] {});
    (void)scheduler.poll();

    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("public_api_headers_compile")
            .declare_dependency("dep")
            .build();
    server::core::app::EngineContext local_context;
    auto runtime_flag = std::make_shared<int>(1);
    local_context.set(runtime_flag);
    runtime.set_service(runtime_flag);
    runtime.set_dependency_ok("dep", true);
    runtime.mark_running();
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
    (void)fps_runtime.stage_input(1, server::core::fps::InputCommand{.input_seq = 1});
    (void)fps_runtime.tick();
    (void)server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
    });
    server::core::fps::UdpSequencedMetrics udp_quality;
    (void)udp_quality.on_packet(1, 1000);
    (void)server::core::mmorpg::evaluate_world_migration(
        server::core::mmorpg::ObservedWorldMigrationWorld{},
        std::nullopt,
        std::nullopt);
    (void)server::core::mmorpg::collect_observed_pools(
        std::vector<server::core::mmorpg::ObservedTopologyInstance>{});
    (void)server::core::mmorpg::plan_topology_actuation(
        server::core::mmorpg::DesiredTopologyDocument{},
        std::vector<server::core::mmorpg::ObservedTopologyPool>{});
    (void)server::core::mmorpg::evaluate_topology_actuation_request_status(
        server::core::mmorpg::TopologyActuationRequestDocument{},
        server::core::mmorpg::DesiredTopologyDocument{},
        std::vector<server::core::mmorpg::ObservedTopologyPool>{});
    (void)server::core::mmorpg::evaluate_topology_actuation_execution_status(
        server::core::mmorpg::TopologyActuationExecutionDocument{},
        server::core::mmorpg::TopologyActuationRequestDocument{},
        server::core::mmorpg::DesiredTopologyDocument{},
        std::vector<server::core::mmorpg::ObservedTopologyPool>{});
    (void)server::core::mmorpg::evaluate_topology_actuation_realization_status(
        server::core::mmorpg::TopologyActuationExecutionDocument{},
        server::core::mmorpg::TopologyActuationRequestDocument{},
        server::core::mmorpg::DesiredTopologyDocument{},
        std::vector<server::core::mmorpg::ObservedTopologyPool>{});
    (void)server::core::mmorpg::evaluate_topology_actuation_adapter_status(
        server::core::mmorpg::TopologyActuationAdapterLeaseDocument{},
        server::core::mmorpg::TopologyActuationExecutionDocument{},
        server::core::mmorpg::TopologyActuationRequestDocument{},
        server::core::mmorpg::DesiredTopologyDocument{},
        std::vector<server::core::mmorpg::ObservedTopologyPool>{});
    (void)server::core::mmorpg::find_topology_actuation_runtime_assignment(
        server::core::mmorpg::TopologyActuationRuntimeAssignmentDocument{},
        "server-1");
    (void)server::core::mmorpg::evaluate_world_transfer(
        server::core::mmorpg::ObservedWorldTransferState{});
    (void)server::core::mmorpg::evaluate_world_drain(
        server::core::mmorpg::ObservedWorldDrainState{});
    (void)server::core::mmorpg::evaluate_world_drain_orchestration(
        server::core::mmorpg::WorldDrainStatus{},
        std::nullopt,
        std::nullopt);
    server::core::fps::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.opcode_allowlist = server::core::fps::parse_direct_opcode_allowlist("0x0206");
    (void)rollout_policy.opcode_allowed(0x0206);
    (void)server::core::fps::evaluate_direct_attach(rollout_policy, "session-a", 1);
    (void)server::core::fps::encode_direct_bind_request_payload(server::core::fps::DirectBindRequest{});

    server::core::JobQueue queue(4);
    server::core::ThreadManager workers(queue);
    workers.Start(1);
    workers.Stop();

    server::core::app::install_termination_signal_handlers();
    (void)server::core::app::termination_signal_received();

    server::core::metrics::MetricsHttpServer metrics_server(0, [] { return std::string{}; });

    server::core::metrics::Gauge& gauge = server::core::metrics::get_gauge("public_api_headers_compile_gauge");
    gauge.set(1.0);

    server::core::BufferManager buffers(128, 2);
    auto pooled = buffers.Acquire();
    (void)pooled;

    server::core::Dispatcher dispatcher;
    dispatcher.register_handler(server::core::protocol::MSG_PING,
                                [](server::core::Session&, std::span<const std::uint8_t>) {});

    server::core::protocol::PacketHeader header{};
    std::array<std::uint8_t, server::core::protocol::k_header_bytes> encoded{};
    server::core::protocol::encode_header(header, encoded.data());
    server::core::protocol::decode_header(encoded.data(), header);

    (void)server::core::api::version_string();
    (void)server::core::build_info::git_hash();
    (void)server::core::runtime_metrics::snapshot();
    (void)server::core::compression::Compressor::get_max_compressed_size(16);
    (void)server::core::security::Cipher::IV_SIZE;
    (void)server::core::protocol::FLAG_COMPRESSED;
    (void)server::core::protocol::CAP_COMPRESS_SUPP;
    (void)server::core::protocol::errc::UNKNOWN_MSG_ID;

    server::core::log::set_level(server::core::log::level::info);
    (void)server::core::util::paths::executable_dir();

    std::ostringstream metrics;
    server::core::metrics::append_build_info(metrics);

    auto connection = std::make_shared<server::core::net::Connection>(hive_ptr);
    server::core::net::Listener listener(
        hive_ptr,
        {boost::asio::ip::address_v4::loopback(), 0},
        [connection](std::shared_ptr<server::core::net::Hive>) { return connection; });
    (void)listener.local_endpoint();

    struct PublicCompileService {
        int value{1};
    };
    auto service = std::make_shared<PublicCompileService>();
    server::core::util::services::set(service);
    server::core::util::services::clear();

    return 0;
}
