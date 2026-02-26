#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <sstream>
#include <string>

#include <boost/asio/io_context.hpp>

#include "server/core/api/version.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/app/termination_signals.hpp"
#include "server/core/build_info.hpp"
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

void scenario_api_and_app() {
    (void)server::core::api::version_string();

    server::core::app::AppHost host{"stable_header_scenarios"};
    host.declare_dependency("dep");
    host.set_dependency_ok("dep", true);
    host.set_ready(true);
    host.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kRunning);

    server::core::app::install_termination_signal_handlers();
    (void)server::core::app::termination_signal_received();
    (void)host.lifecycle_metrics_text();
    (void)host.dependency_metrics_text();
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

    server::core::metrics::Counter& counter = server::core::metrics::get_counter("stable_header_counter");
    counter.inc();

    server::core::metrics::Gauge& gauge = server::core::metrics::get_gauge("stable_header_gauge");
    gauge.set(1.0);

    std::ostringstream metrics;
    server::core::metrics::append_build_info(metrics);
    (void)server::core::runtime_metrics::snapshot();
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
    auto service = std::make_shared<StableScenarioService>();
    server::core::util::services::set(service);
    (void)server::core::util::services::get<StableScenarioService>();
    server::core::util::services::clear();
}

}  // namespace

int main() {
    scenario_api_and_app();
    scenario_concurrency_and_config();
    scenario_metrics_and_runtime();
    scenario_protocol_and_security();
    scenario_net_and_utils();
    return 0;
}
