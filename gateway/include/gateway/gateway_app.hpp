#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include "gateway/direct_egress_route.hpp"
#include "gateway/rudp_rollout_policy.hpp"
#include "gateway/resilience_controls.hpp"
#include "gateway/transport_session.hpp"
#include "gateway/udp_bind_abuse_guard.hpp"
#include "server/core/app/engine_runtime.hpp"
#include "server/core/realtime/direct_bind.hpp"
#include "server/core/net/listener.hpp"
#include "server/core/discovery/instance_registry.hpp"

namespace gateway::auth {
class IAuthenticator;
}

namespace gateway {
class SessionDirectory;
}

namespace server::core::net {
class Hive;
}

namespace server::core::storage::redis {
class IRedisClient;
}

namespace server::core::net::rudp {
struct RudpConfig;
}

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

    std::string gateway_id() const { return gateway_id_; }
    bool allow_anonymous() const noexcept { return allow_anonymous_; }

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    std::shared_ptr<server::core::net::TransportListener> listener_;
    server::core::app::EngineRuntime engine_;
    server::core::app::AppHost& app_host_;
    std::shared_ptr<auth::IAuthenticator> authenticator_;
    std::string gateway_id_;
    std::string listen_host_;
    std::uint16_t listen_port_{6000};
    bool allow_anonymous_{true};

    void record_connection_accept() {
        (void)connections_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_resolve_fail() {
        (void)backend_resolve_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_connect_fail() {
        (void)backend_connect_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_connect_timeout() {
        (void)backend_connect_timeout_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_write_error() {
        (void)backend_write_error_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_send_queue_overflow() {
        (void)backend_send_queue_overflow_total_.fetch_add(1, std::memory_order_relaxed);
    }

    IngressAdmission admit_ingress_connection();
    static const char* ingress_admission_name(IngressAdmission admission) noexcept;

    bool backend_circuit_allows_connect();
    void record_backend_connect_success_event();
    void record_backend_connect_failure_event();

    bool consume_backend_retry_budget();
    std::chrono::milliseconds backend_retry_delay(std::uint32_t attempt) const;
    std::uint32_t udp_bind_retry_delay_ms(std::uint32_t attempt) const;

    void record_backend_retry_scheduled() {
        (void)backend_connect_retry_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_retry_budget_exhausted() {
        (void)backend_retry_budget_exhausted_total_.fetch_add(1, std::memory_order_relaxed);
    }

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

    struct SessionState;
    std::mutex session_mutex_;
    std::unordered_map<std::string, std::unique_ptr<SessionState>> sessions_;

    std::atomic<std::uint64_t> connections_total_{0};
    std::atomic<std::uint64_t> backend_resolve_fail_total_{0};
    std::atomic<std::uint64_t> backend_connect_fail_total_{0};
    std::atomic<std::uint64_t> backend_connect_timeout_total_{0};
    std::atomic<std::uint64_t> backend_write_error_total_{0};
    std::atomic<std::uint64_t> backend_send_queue_overflow_total_{0};
    std::atomic<std::uint64_t> backend_circuit_open_total_{0};
    std::atomic<std::uint64_t> backend_circuit_reject_total_{0};
    std::atomic<std::uint64_t> backend_connect_retry_total_{0};
    std::atomic<std::uint64_t> backend_retry_budget_exhausted_total_{0};
    std::atomic<std::uint64_t> resume_routing_bind_total_{0};
    std::atomic<std::uint64_t> resume_routing_hit_total_{0};
    std::atomic<std::uint64_t> resume_locator_bind_total_{0};
    std::atomic<std::uint64_t> resume_locator_lookup_hit_total_{0};
    std::atomic<std::uint64_t> resume_locator_lookup_miss_total_{0};
    std::atomic<std::uint64_t> resume_locator_selector_hit_total_{0};
    std::atomic<std::uint64_t> resume_locator_selector_fallback_total_{0};
    std::atomic<std::uint64_t> world_policy_filtered_sticky_total_{0};
    std::atomic<std::uint64_t> world_policy_filtered_candidate_total_{0};
    std::atomic<std::uint64_t> world_policy_replacement_selected_total_{0};

    std::atomic<std::uint64_t> ingress_reject_not_ready_total_{0};
    std::atomic<std::uint64_t> ingress_reject_rate_limit_total_{0};
    std::atomic<std::uint64_t> ingress_reject_session_limit_total_{0};
    std::atomic<std::uint64_t> ingress_reject_circuit_open_total_{0};

    std::string boot_id_;
    std::uint16_t metrics_port_{6001};
    std::uint32_t backend_connect_timeout_ms_{5000};
    std::size_t backend_send_queue_max_bytes_{256 * 1024};
    std::uint32_t backend_connect_retry_budget_per_min_{120};
    std::uint32_t backend_connect_retry_backoff_ms_{200};
    std::uint32_t backend_connect_retry_backoff_max_ms_{2000};
    bool backend_circuit_breaker_enabled_{true};
    std::uint32_t backend_circuit_fail_threshold_{5};
    std::uint32_t backend_circuit_open_ms_{10000};

    std::uint32_t ingress_tokens_per_sec_{200};
    std::uint32_t ingress_burst_tokens_{400};
    std::size_t ingress_max_active_sessions_{50000};

    gateway::TokenBucket ingress_token_bucket_{};
    gateway::RetryBudget backend_retry_budget_{};
    gateway::CircuitBreaker backend_circuit_breaker_{};

    std::string udp_listen_host_;
    std::uint16_t udp_listen_port_{0};
    std::string udp_bind_secret_;
    std::uint32_t udp_bind_ttl_ms_{5000};
    std::uint32_t udp_bind_fail_window_ms_{10000};
    std::uint32_t udp_bind_fail_limit_{5};
    std::uint32_t udp_bind_block_ms_{60000};
    std::uint32_t udp_bind_retry_backoff_ms_{200};
    std::uint32_t udp_bind_retry_backoff_max_ms_{2000};
    std::uint32_t udp_bind_retry_max_attempts_{6};
    std::unordered_set<std::uint16_t> udp_opcode_allowlist_{};
    RudpRolloutPolicy rudp_rollout_policy_{};
    std::unique_ptr<server::core::net::rudp::RudpConfig> rudp_config_;
    UdpBindAbuseGuard udp_bind_abuse_guard_;
    std::unique_ptr<boost::asio::ip::udp::socket> udp_socket_;
    boost::asio::ip::udp::endpoint udp_remote_endpoint_;
    std::array<std::uint8_t, 2048> udp_read_buffer_{};

    // 상태 및 저장소
    std::shared_ptr<server::core::storage::redis::IRedisClient> redis_client_;
    std::shared_ptr<server::core::discovery::IInstanceStateBackend> backend_registry_;
    std::unique_ptr<SessionDirectory> session_directory_;
    std::string redis_uri_;
    std::string continuity_prefix_;
    std::string session_directory_prefix_{"gateway/session/"};
    std::string resume_locator_prefix_{"gateway/session/locator/"};
    std::uint32_t resume_locator_ttl_sec_{900};

    std::atomic<bool> infra_probe_stop_{false};
    std::thread infra_probe_thread_;
    std::atomic<std::uint64_t> udp_packets_total_{0};
    std::atomic<std::uint64_t> udp_receive_error_total_{0};
    std::atomic<std::uint64_t> udp_send_error_total_{0};
    std::atomic<std::uint64_t> udp_bind_ticket_issued_total_{0};
    std::atomic<std::uint64_t> udp_bind_success_total_{0};
    std::atomic<std::uint64_t> udp_bind_reject_total_{0};
    std::atomic<std::uint64_t> udp_bind_block_total_{0};
    std::atomic<std::uint64_t> udp_bind_rate_limit_reject_total_{0};
    std::atomic<std::uint64_t> udp_bind_retry_backoff_total_{0};
    std::atomic<std::uint64_t> udp_bind_retry_reject_total_{0};
    std::atomic<std::uint64_t> udp_opcode_allowlist_reject_total_{0};
    std::atomic<std::uint64_t> udp_forward_total_{0};
    std::atomic<std::uint64_t> udp_forward_reliable_ordered_total_{0};
    std::atomic<std::uint64_t> udp_forward_reliable_total_{0};
    std::atomic<std::uint64_t> udp_forward_unreliable_sequenced_total_{0};
    std::atomic<std::uint64_t> udp_direct_state_delta_total_{0};
    std::atomic<std::uint64_t> udp_replay_drop_total_{0};
    std::atomic<std::uint64_t> udp_reorder_drop_total_{0};
    std::atomic<std::uint64_t> udp_duplicate_drop_total_{0};
    std::atomic<std::uint64_t> udp_retransmit_total_{0};
    std::atomic<std::uint64_t> udp_loss_estimated_total_{0};
    std::atomic<std::uint64_t> udp_jitter_ms_last_{0};
    std::atomic<std::uint64_t> udp_rtt_ms_last_{0};
    std::atomic<std::uint64_t> rudp_packets_total_{0};
    std::atomic<std::uint64_t> rudp_packets_reject_total_{0};
    std::atomic<std::uint64_t> rudp_inner_forward_total_{0};
    std::atomic<std::uint64_t> rudp_fallback_total_{0};
    std::atomic<std::uint64_t> rudp_direct_state_delta_total_{0};
    std::atomic<std::uint64_t> direct_state_delta_tcp_fallback_total_{0};
    std::atomic<bool> udp_enabled_{false};
};

} // namespace gateway
