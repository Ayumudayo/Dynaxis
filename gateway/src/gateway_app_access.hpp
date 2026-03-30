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
#include <boost/asio/ip/udp.hpp>

#include "gateway/transport_session.hpp"
#include "server/core/discovery/instance_registry.hpp"
#include "server/core/realtime/direct_bind.hpp"

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

    using UdpBindTicket = server::core::realtime::DirectBindTicket;

    struct CreatedBackendSession {
        TransportSessionPtr session;
        std::string session_id;
        std::string backend_instance_id;
    };

    using ParsedUdpBindRequest = server::core::realtime::DirectBindRequest;

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
    static std::uint32_t udp_bind_retry_delay_ms(const GatewayApp& app, std::uint32_t attempt);

    static void start_infrastructure_probe(GatewayApp& app);
    static void stop_infrastructure_probe(GatewayApp& app);

    static std::optional<CreatedBackendSession> create_backend_connection(
        GatewayApp& app,
        const std::string& client_id,
        std::weak_ptr<GatewayConnection> connection);
    static void close_backend_connection(GatewayApp& app, const std::string& session_id);

    static std::string make_udp_bind_token(const GatewayApp& app,
                                           std::string_view session_id,
                                           std::uint64_t nonce,
                                           std::uint64_t expires_unix_ms);
    static std::vector<std::uint8_t> make_udp_bind_res_frame(const GatewayApp& app,
                                                             std::uint16_t code,
                                                             const UdpBindTicket& ticket,
                                                             std::string_view message,
                                                             std::uint32_t seq = 0);
    static std::vector<std::uint8_t> make_udp_bind_res_frame(const GatewayApp& app,
                                                             std::uint16_t code,
                                                             std::string_view session_id,
                                                             std::uint64_t nonce,
                                                             std::uint64_t expires_unix_ms,
                                                             std::string_view token,
                                                             std::string_view message,
                                                             std::uint32_t seq = 0);
    static std::optional<std::vector<std::uint8_t>> make_udp_bind_ticket_frame(GatewayApp& app,
                                                                                const std::string& session_id);
    static bool parse_udp_bind_req(const GatewayApp& app,
                                   std::span<const std::uint8_t> payload,
                                   ParsedUdpBindRequest& out);
    static std::uint16_t apply_udp_bind_request(GatewayApp& app,
                                                const ParsedUdpBindRequest& req,
                                                const boost::asio::ip::udp::endpoint& endpoint,
                                                UdpBindTicket& applied_ticket,
                                                std::string& message);
    static void send_udp_datagram(GatewayApp& app,
                                  std::vector<std::uint8_t> frame,
                                  const boost::asio::ip::udp::endpoint& endpoint);
    static bool try_send_direct_client_frame(GatewayApp& app,
                                             std::string_view session_id,
                                             std::uint16_t msg_id,
                                             std::span<const std::uint8_t> frame);
    static void trace_udp_bind_send(const GatewayApp& app,
                                    std::span<const std::uint8_t> frame,
                                    const boost::asio::ip::udp::endpoint& endpoint);

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
    static void start_udp_listener(GatewayApp& app);
    static void stop_udp_listener(GatewayApp& app);
    static void do_udp_receive(GatewayApp& app);
};

} // namespace gateway
