#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
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
#include "server/core/worlds/topology.hpp"
#include "server/core/worlds/world_drain.hpp"
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
#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/scripting/lua_sandbox.hpp"
#include "server/core/scripting/script_watcher.hpp"
#include "server/core/security/cipher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/paths.hpp"
#include "server/core/util/service_registry.hpp"

namespace {

class PublicCompileUnitOfWork final : public server::core::storage_execution::IUnitOfWork {
public:
    void commit() override {}
    void rollback() override {}
};

class PublicCompileConnectionPool final : public server::core::storage_execution::IConnectionPool {
public:
    std::unique_ptr<server::core::storage_execution::IUnitOfWork> make_unit_of_work() override {
        return std::make_unique<PublicCompileUnitOfWork>();
    }

    bool health_check() override { return true; }
};

struct PublicCompilePluginApi {
    std::uint32_t abi_version{1};
    const char* name{"public_api_headers_compile"};
};

} // namespace

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
    server::core::realtime::WorldRuntime fps_runtime;
    (void)fps_runtime.stage_input(1, server::core::realtime::InputCommand{.input_seq = 1});
    (void)fps_runtime.tick();
    (void)server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
    });
    server::core::realtime::UdpSequencedMetrics udp_quality;
    (void)udp_quality.on_packet(1, 1000);
    (void)server::core::worlds::evaluate_world_migration(
        server::core::worlds::ObservedWorldMigrationWorld{},
        std::nullopt,
        std::nullopt);
    server::core::discovery::InMemoryStateBackend discovery_backend;
    (void)discovery_backend.upsert(server::core::discovery::InstanceRecord{.instance_id = "server-a"});
    (void)server::core::discovery::select_instances(
        discovery_backend.list_instances(),
        server::core::discovery::InstanceSelector{.all = true});
    (void)server::core::discovery::serialize_world_lifecycle_policy(
        server::core::discovery::WorldLifecyclePolicy{.draining = true});
    auto storage_pool = std::make_shared<PublicCompileConnectionPool>();
    server::core::storage_execution::DbWorkerPool storage_workers(storage_pool, 4);
    (void)storage_workers.running();
    (void)server::core::storage_execution::retry_backoff_upper_bound_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kLinear,
            .base_delay_ms = 50,
            .max_delay_ms = 200,
        },
        2);
    std::mt19937_64 retry_rng(3);
    (void)server::core::storage_execution::sample_retry_backoff_delay_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kExponentialFullJitter,
            .base_delay_ms = 100,
            .max_delay_ms = 800,
        },
        1,
        retry_rng);
    server::core::plugin::SharedLibrary plugin_library;
    using PublicCompilePluginHost = server::core::plugin::PluginHost<PublicCompilePluginApi>;
    PublicCompilePluginHost::Config plugin_host_cfg{};
    plugin_host_cfg.plugin_path = std::filesystem::path{"public_compile_plugin"};
    plugin_host_cfg.cache_dir = std::filesystem::temp_directory_path() / "public_compile_plugin_cache";
    plugin_host_cfg.entrypoint_symbol = "public_compile_plugin_api_v1";
    plugin_host_cfg.api_resolver = [](void*, std::string&) -> const PublicCompilePluginApi* { return nullptr; };
    plugin_host_cfg.api_validator = [](const PublicCompilePluginApi*, std::string&) { return true; };
    plugin_host_cfg.instance_creator = [](const PublicCompilePluginApi*, std::string&) -> void* { return nullptr; };
    plugin_host_cfg.instance_destroyer = [](const PublicCompilePluginApi*, void*) {};
    PublicCompilePluginHost plugin_host(std::move(plugin_host_cfg));
    using PublicCompilePluginChain = server::core::plugin::PluginChainHost<PublicCompilePluginApi>;
    PublicCompilePluginChain::Config plugin_chain_cfg{};
    plugin_chain_cfg.cache_dir = std::filesystem::temp_directory_path() / "public_compile_plugin_chain_cache";
    PublicCompilePluginChain plugin_chain(std::move(plugin_chain_cfg));
    server::core::scripting::ScriptWatcher::Config watcher_cfg{};
    watcher_cfg.scripts_dir = std::filesystem::temp_directory_path() / "public_compile_scripts";
    watcher_cfg.extensions = {".lua"};
    server::core::scripting::ScriptWatcher watcher(watcher_cfg);
    const auto sandbox_policy = server::core::scripting::sandbox::default_policy();
    server::core::scripting::LuaRuntime lua_runtime;
    (void)plugin_library.is_loaded();
    (void)plugin_host.metrics_snapshot();
    (void)plugin_chain.metrics_snapshot();
    (void)watcher;
    (void)server::core::scripting::sandbox::is_library_allowed("math", sandbox_policy);
    (void)lua_runtime.enabled();
    (void)server::core::worlds::collect_observed_pools(
        std::vector<server::core::worlds::ObservedTopologyInstance>{});
    (void)server::core::worlds::plan_topology_actuation(
        server::core::worlds::DesiredTopologyDocument{},
        std::vector<server::core::worlds::ObservedTopologyPool>{});
    (void)server::core::worlds::evaluate_topology_actuation_request_status(
        server::core::worlds::TopologyActuationRequestDocument{},
        server::core::worlds::DesiredTopologyDocument{},
        std::vector<server::core::worlds::ObservedTopologyPool>{});
    (void)server::core::worlds::evaluate_topology_actuation_execution_status(
        server::core::worlds::TopologyActuationExecutionDocument{},
        server::core::worlds::TopologyActuationRequestDocument{},
        server::core::worlds::DesiredTopologyDocument{},
        std::vector<server::core::worlds::ObservedTopologyPool>{});
    (void)server::core::worlds::evaluate_topology_actuation_realization_status(
        server::core::worlds::TopologyActuationExecutionDocument{},
        server::core::worlds::TopologyActuationRequestDocument{},
        server::core::worlds::DesiredTopologyDocument{},
        std::vector<server::core::worlds::ObservedTopologyPool>{});
    (void)server::core::worlds::evaluate_topology_actuation_adapter_status(
        server::core::worlds::TopologyActuationAdapterLeaseDocument{},
        server::core::worlds::TopologyActuationExecutionDocument{},
        server::core::worlds::TopologyActuationRequestDocument{},
        server::core::worlds::DesiredTopologyDocument{},
        std::vector<server::core::worlds::ObservedTopologyPool>{});
    (void)server::core::worlds::find_topology_actuation_runtime_assignment(
        server::core::worlds::TopologyActuationRuntimeAssignmentDocument{},
        "server-1");
    (void)server::core::worlds::evaluate_world_transfer(
        server::core::worlds::ObservedWorldTransferState{});
    (void)server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{});
    (void)server::core::worlds::evaluate_world_drain_orchestration(
        server::core::worlds::WorldDrainStatus{},
        std::nullopt,
        std::nullopt);
    auto kubernetes_binding = server::core::worlds::make_kubernetes_pool_binding(
        server::core::worlds::DesiredTopologyPool{
            .world_id = "starter-a",
            .shard = "alpha",
            .replicas = 2,
        });
    server::core::worlds::TopologyActuationRuntimeAssignmentDocument assignment_document{};
    (void)server::core::worlds::count_topology_actuation_runtime_assignments(
        assignment_document,
        "starter-a",
        "alpha",
        server::core::worlds::TopologyActuationActionKind::kScaleOutPool);
    (void)server::core::worlds::evaluate_kubernetes_pool_orchestration(
        kubernetes_binding,
        server::core::worlds::KubernetesPoolObservation{},
        std::nullopt,
        std::nullopt);
    server::core::realtime::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.opcode_allowlist = server::core::realtime::parse_direct_opcode_allowlist("0x0206");
    (void)rollout_policy.opcode_allowed(0x0206);
    (void)server::core::realtime::evaluate_direct_attach(rollout_policy, "session-a", 1);
    (void)server::core::realtime::encode_direct_bind_request_payload(server::core::realtime::DirectBindRequest{});

    server::core::JobQueue queue(4);
    server::core::ThreadManager workers(queue);
    workers.Start(1);
    workers.Stop();

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
    struct PublicCompilePeerService {
        int value{2};
    };
    auto peer_runtime =
        server::core::app::EngineBuilder("public_api_headers_compile_peer").build();
    auto service = std::make_shared<PublicCompileService>();
    auto peer_service = std::make_shared<PublicCompilePeerService>();
    runtime.bridge_service(service);
    peer_runtime.bridge_service(peer_service);
    (void)runtime.snapshot();
    (void)peer_runtime.snapshot();
    runtime.clear_global_services();
    peer_runtime.clear_global_services();

    return 0;
}
