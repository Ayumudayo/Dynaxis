#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "gateway/transport_session.hpp"
#include "server/core/discovery/instance_registry.hpp"

namespace gateway {

class GatewayApp;
class GatewayConnection;

struct GatewayAppAccess {
    enum class IngressAdmission {
        kAccept = 0,
        kRejectNotReady,
        kRejectRateLimited,
        kRejectSessionLimit,
        kRejectCircuitOpen,
    };

    struct SelectedBackend {
        server::core::discovery::InstanceRecord record;
        bool sticky_hit{false};
    };


    struct CreatedBackendSession {
        TransportSessionPtr session;
        std::string session_id;
        std::string backend_instance_id;
    };


    static boost::asio::io_context& io_context(GatewayApp& app);
    static std::string gateway_id(const GatewayApp& app);
    static bool allow_anonymous(const GatewayApp& app) noexcept;
    static std::string render_metrics_text(GatewayApp& app);

    static void record_connection_accept(GatewayApp& app);
    static void record_backend_resolve_fail(GatewayApp& app);
    static void record_backend_connect_fail(GatewayApp& app);
    static void record_backend_connect_timeout(GatewayApp& app);
    static void record_backend_write_error(GatewayApp& app);
    static void record_backend_send_queue_overflow(GatewayApp& app);
    static void record_backend_retry_scheduled(GatewayApp& app);
    static void record_backend_retry_budget_exhausted(GatewayApp& app);

    static IngressAdmission admit_ingress_connection(GatewayApp& app);
    static const char* ingress_admission_name(IngressAdmission admission) noexcept;

    static bool backend_circuit_allows_connect(GatewayApp& app);
    static void record_backend_connect_success_event(GatewayApp& app);
    static void record_backend_connect_failure_event(GatewayApp& app);

    static bool consume_backend_retry_budget(GatewayApp& app);
    static std::chrono::milliseconds backend_retry_delay(const GatewayApp& app, std::uint32_t attempt);

    static void start_infrastructure_probe(GatewayApp& app);
    static void stop_infrastructure_probe(GatewayApp& app);

    static std::optional<CreatedBackendSession> create_backend_connection(
        GatewayApp& app,
        const std::string& client_id,
        std::weak_ptr<GatewayConnection> connection);
    static void close_backend_connection(GatewayApp& app, const std::string& session_id);

    static std::optional<SelectedBackend> select_best_server(GatewayApp& app,
                                                             const std::string& client_id = "");
    static void register_resume_routing_key(GatewayApp& app,
                                            const std::string& routing_key,
                                            const std::string& backend_instance_id);
    static std::string make_resume_locator_key(const GatewayApp& app, std::string_view routing_key);
    static std::optional<server::core::discovery::InstanceSelector> load_resume_locator_selector(
        GatewayApp& app,
        std::string_view routing_key);
    static void persist_resume_locator_hint(GatewayApp& app,
                                            std::string_view routing_key,
                                            const server::core::discovery::InstanceRecord& record);
    static void on_backend_connected(GatewayApp& app,
                                     const std::string& client_id,
                                     const std::string& backend_instance_id,
                                     bool sticky_hit);

    static void configure_gateway(GatewayApp& app);
    static void configure_infrastructure(GatewayApp& app);
    static void start_listener(GatewayApp& app);
};

} // namespace gateway
