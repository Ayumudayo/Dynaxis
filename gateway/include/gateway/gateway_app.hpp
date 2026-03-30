#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/ip/udp.hpp>

#include "gateway/transport_session.hpp"
#include "server/core/realtime/direct_bind.hpp"
#include "server/core/discovery/instance_registry.hpp"

namespace gateway {

class GatewayConnection;
class BackendConnection;

/**
 * @brief gateway 프로세스의 엣지 유입(edge ingress), 백엔드(backend) 선택, direct-transport 게이트를 조율하는 메인 오케스트레이터입니다.
 *
 * 이 타입은 TCP 리스너로 클라이언트 연결을 수락하고, Redis Instance Registry를 바탕으로
 * 백엔드(`server_app`)를 선택해 TCP 브리지를 구성합니다. 동시에 연결 타임아웃(connect timeout),
 * 재시도 예산(retry budget), circuit breaker, UDP bind abuse guard 같은 보호 장치를 한곳에 모아,
 * 엣지 트래픽 문제와 백엔드 비즈니스 로직을 분리합니다.
 */
class GatewayApp {
public:
    GatewayApp();
    ~GatewayApp();

    /**
     * @brief gateway 메인 루프를 실행합니다.
     * @return 종료 코드(0이면 정상 종료)
     */
    int run();

    /** @brief gateway 전체 종료를 요청합니다. */
    void stop();

 private:
    enum class IngressAdmission {
        kAccept = 0,
        kRejectNotReady,
        kRejectRateLimited,
        kRejectSessionLimit,
        kRejectCircuitOpen,
    };

    /** @brief 내부 backend 선택 결과와 sticky 적중 여부를 함께 들고 다니는 구현용 값 객체입니다. */
    struct SelectedBackend {
        server::core::discovery::InstanceRecord record;
        bool sticky_hit{false};
    };

    using UdpBindTicket = server::core::realtime::DirectBindTicket;

    /** @brief 생성된 backend 브리지 세션과 gateway 내부 식별자를 함께 반환하는 구현용 값 객체입니다. */
    struct CreatedBackendSession {
        TransportSessionPtr session;
        std::string session_id;
        std::string backend_instance_id;
    };

    friend class GatewayConnection;
    friend class BackendConnection;

    struct Impl;

    std::string gateway_id() const;
    bool allow_anonymous() const noexcept;

    void record_connection_accept();
    void record_backend_resolve_fail();
    void record_backend_connect_fail();
    void record_backend_connect_timeout();
    void record_backend_write_error();
    void record_backend_send_queue_overflow();

    IngressAdmission admit_ingress_connection();
    static const char* ingress_admission_name(IngressAdmission admission) noexcept;

    bool backend_circuit_allows_connect();
    void record_backend_connect_success_event();
    void record_backend_connect_failure_event();

    bool consume_backend_retry_budget();
    std::chrono::milliseconds backend_retry_delay(std::uint32_t attempt) const;
    std::uint32_t udp_bind_retry_delay_ms(std::uint32_t attempt) const;

    void record_backend_retry_scheduled();
    void record_backend_retry_budget_exhausted();

    std::optional<CreatedBackendSession> create_backend_connection(
        const std::string& client_id,
        std::weak_ptr<GatewayConnection> connection);
    void close_backend_connection(const std::string& session_id);
    std::optional<SelectedBackend> select_best_server(const std::string& client_id = "");
    void register_resume_routing_key(const std::string& routing_key,
                                     const std::string& backend_instance_id);
    std::optional<std::vector<std::uint8_t>> make_udp_bind_ticket_frame(const std::string& session_id);
    bool try_send_direct_client_frame(std::string_view session_id,
                                      std::uint16_t msg_id,
                                      std::span<const std::uint8_t> frame);

    void on_backend_connected(const std::string& client_id,
                              const std::string& backend_instance_id,
                              bool sticky_hit);
    std::string make_resume_locator_key(std::string_view routing_key) const;
    std::optional<server::core::discovery::InstanceSelector> load_resume_locator_selector(
        std::string_view routing_key);
    void persist_resume_locator_hint(std::string_view routing_key,
                                     const server::core::discovery::InstanceRecord& record);
    void configure_gateway();
    void configure_infrastructure();
    void start_listener();
    void start_udp_listener();
    void stop_udp_listener();
    void do_udp_receive();

    /** @brief UDP bind 요청 payload 파싱 결과입니다. */
    using ParsedUdpBindRequest = server::core::realtime::DirectBindRequest;

    std::vector<std::uint8_t> make_udp_bind_res_frame(std::uint16_t code,
                                                      const UdpBindTicket& ticket,
                                                      std::string_view message,
                                                      std::uint32_t seq = 0) const;
    std::vector<std::uint8_t> make_udp_bind_res_frame(std::uint16_t code,
                                                      std::string_view session_id,
                                                      std::uint64_t nonce,
                                                      std::uint64_t expires_unix_ms,
                                                      std::string_view token,
                                                      std::string_view message,
                                                      std::uint32_t seq = 0) const;
    void trace_udp_bind_send(std::span<const std::uint8_t> frame,
                             const boost::asio::ip::udp::endpoint& endpoint) const;
    bool parse_udp_bind_req(std::span<const std::uint8_t> payload, ParsedUdpBindRequest& out) const;
    std::uint16_t apply_udp_bind_request(const ParsedUdpBindRequest& req,
                                         const boost::asio::ip::udp::endpoint& endpoint,
                                         UdpBindTicket& applied_ticket,
                                         std::string& message);
    std::string make_udp_bind_token(std::string_view session_id,
                                    std::uint64_t nonce,
                                    std::uint64_t expires_unix_ms) const;
    void send_udp_datagram(std::vector<std::uint8_t> frame,
                           const boost::asio::ip::udp::endpoint& endpoint);

    void start_infrastructure_probe();
    void stop_infrastructure_probe();

    std::unique_ptr<Impl> impl_;
};

} // namespace gateway
