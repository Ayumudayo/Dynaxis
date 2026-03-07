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
#include <vector>
#include <deque>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "gateway/auth/authenticator.hpp"
#include "gateway/rudp_rollout_policy.hpp"
#include "gateway/resilience_controls.hpp"
#include "gateway/udp_bind_abuse_guard.hpp"
#include "gateway/udp_sequenced_metrics.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"
#include "server/core/net/rudp/rudp_engine.hpp"
#include "server/state/instance_registry.hpp"
#include "server/storage/redis/client.hpp"
#include "gateway/session_directory.hpp"

namespace gateway {

class GatewayConnection;

/**
 * @brief Gateway 애플리케이션의 메인 오케스트레이터입니다.
 *
 * TCP 리스너로 클라이언트 연결을 수락하고,
 * Redis Instance Registry를 바탕으로 backend(`server_app`)를 선택해 TCP 브리지를 구성합니다.
 */
class GatewayApp {
public:
    /**
     * @brief Gateway 세션이 backend로 payload를 전달하기 위한 전송 인터페이스입니다.
     *
     * TCP/UDP ingress 경로는 이 인터페이스만 사용해 backend forward를 수행하고,
     * 실제 전송 구현(TCP bridge, 향후 UDP/RUDP direct path)은 구현체로 분리합니다.
     */
    class ITransportSession {
    public:
        virtual ~ITransportSession() = default;
        virtual void send(std::vector<std::uint8_t> payload) = 0;
        virtual void send(const std::uint8_t* data, std::size_t length) = 0;
        virtual void close() = 0;
        virtual const std::string& session_id() const = 0;
    };
    using TransportSessionPtr = std::shared_ptr<ITransportSession>;

    enum class IngressAdmission {
        kAccept = 0,
        kRejectNotReady,
        kRejectRateLimited,
        kRejectSessionLimit,
        kRejectCircuitOpen,
    };

    /** @brief backend 선택 결과입니다. */
    struct SelectedBackend {
        server::state::InstanceRecord record;
        bool sticky_hit{false};
    };

    /** @brief TCP 응답으로 전달되는 UDP bind ticket 정보입니다. */
    struct UdpBindTicket {
        std::string session_id;           ///< gateway 내부 세션 ID
        std::uint64_t nonce{0};           ///< bind 논스 값
        std::uint64_t expires_unix_ms{0}; ///< ticket 만료 시각(Epoch ms)
        std::string token;                ///< bind 검증 토큰
    };

    /**
     * @brief backend 서버와의 TCP 연결을 관리하는 내부 클래스입니다.
     *
     * `GatewayConnection`(클라이언트)과 game server 사이에서
     * payload를 양방향 브리지합니다.
     */
    class BackendConnection : public ITransportSession,
                              public std::enable_shared_from_this<BackendConnection> {
    public:
        /**
         * @brief backend 연결을 생성합니다.
         * @param app 상위 GatewayApp 참조
         * @param session_id gateway 내부 세션 ID
         * @param client_id 클라이언트 식별자
         * @param backend_instance_id 선택된 backend 인스턴스 ID
         * @param sticky_hit sticky 라우팅 적중 여부
         * @param connection 클라이언트 연결 약한 포인터
         * @param send_queue_max_bytes backend 송신 큐 최대 바이트
         * @param connect_timeout backend 연결 타임아웃
         */
        BackendConnection(GatewayApp& app,
                       std::string session_id,
                       std::string client_id,
                       std::string backend_instance_id,
                       bool sticky_hit,
                       std::weak_ptr<GatewayConnection> connection,
                       std::size_t send_queue_max_bytes,
                       std::chrono::milliseconds connect_timeout);
        ~BackendConnection();

        /**
         * @brief backend로 TCP 연결을 시작합니다.
         * @param host backend 호스트
         * @param port backend 포트
         */
        void connect(const std::string& host, std::uint16_t port);

        /**
         * @brief backend로 payload를 비동기 전송 큐에 넣습니다.
         * @param payload 전송할 바이트 배열
         */
        void send(std::vector<std::uint8_t> payload) override;

        /**
         * @brief backend로 raw bytes를 복사해 비동기 전송 큐에 넣습니다.
         * @param data 전송할 바이트 포인터
         * @param length 전송할 바이트 길이
         */
        void send(const std::uint8_t* data, std::size_t length) override;

        /** @brief backend 연결을 종료합니다. */
        void close() override;

        /**
         * @brief gateway 내부 세션 ID를 반환합니다.
         * @return backend 연결과 매핑된 세션 ID
         */
        const std::string& session_id() const override;

    private:
        void do_connect(const std::string& host, std::uint16_t port);
        void do_read();
        void on_read(const boost::system::error_code& ec, std::size_t bytes_transferred);
        void do_write();
        void on_connect_timeout();

        GatewayApp& app_;
        std::string session_id_;
        std::string client_id_;
        std::string backend_instance_id_;
        bool sticky_hit_{false};
        std::weak_ptr<GatewayConnection> connection_;
        boost::asio::ip::tcp::socket socket_;
        boost::asio::steady_timer connect_timer_;
        boost::asio::steady_timer retry_timer_;
        std::array<std::uint8_t, 8192> buffer_;
        std::atomic<bool> closed_{false};

        std::string connect_host_;
        std::uint16_t connect_port_{0};
        std::uint32_t retry_attempt_{0};
        
        std::mutex send_mutex_;
        std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
        std::size_t queued_bytes_{0};
        std::size_t send_queue_max_bytes_{256 * 1024};
        std::chrono::milliseconds connect_timeout_{5000};
        bool connected_{false};
        bool write_in_progress_{false};

        bool schedule_connect_retry(const char* reason);
        void close_socket_for_retry();
    };
    using BackendConnectionPtr = std::shared_ptr<BackendConnection>;

    GatewayApp();
    ~GatewayApp();

    /**
     * @brief gateway 메인 루프를 실행합니다.
     * @return 종료 코드(0이면 정상 종료)
     */
    int run();

    /** @brief gateway 전체 종료를 요청합니다. */
    void stop();

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

    /**
     * @brief backend 연결을 생성하고 등록합니다.
     * @param client_id 클라이언트 식별자
     * @param connection 연결 weak 포인터
     * @return 생성된 backend 연결
     */
    BackendConnectionPtr create_backend_connection(const std::string& client_id,
                                             std::weak_ptr<GatewayConnection> connection);

    /**
     * @brief session_id에 해당하는 backend 연결을 닫고 제거합니다.
     * @param session_id gateway 내부 세션 ID
     */
    void close_backend_connection(const std::string& session_id);
    
    /**
     * @brief Redis Registry에서 최적 backend를 선택합니다.
     * @param client_id sticky 조회에 사용할 클라이언트 식별자
     * @return 선택된 backend 정보, 후보가 없으면 std::nullopt
     */
    std::optional<SelectedBackend> select_best_server(const std::string& client_id = "");

    /** @brief gateway 인스턴스 ID를 반환합니다. */
    std::string gateway_id() const { return gateway_id_; }

    /** @brief 익명 로그인 허용 여부를 반환합니다. */
    bool allow_anonymous() const noexcept { return allow_anonymous_; }

    /**
     * @brief 세션용 UDP bind ticket을 발급하고 TCP 응답 프레임을 생성합니다.
     * @param session_id gateway 내부 세션 ID
     * @return 생성된 `MSG_UDP_BIND_RES` 프레임, 발급 불가 시 std::nullopt
     */
    std::optional<std::vector<std::uint8_t>> make_udp_bind_ticket_frame(const std::string& session_id);

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    std::shared_ptr<server::core::net::TransportListener> listener_;
    server::core::app::AppHost app_host_{"gateway_app"};
    std::shared_ptr<auth::IAuthenticator> authenticator_;
    std::string gateway_id_;
    std::string listen_host_;
    std::uint16_t listen_port_{6000};
    bool allow_anonymous_{true};

 private:
     void on_backend_connected(const std::string& client_id,
                               const std::string& backend_instance_id,
                               bool sticky_hit);
     void configure_gateway();
     void configure_infrastructure();
     void start_listener();
     void start_udp_listener();
     void stop_udp_listener();
     void do_udp_receive();

     /** @brief UDP bind 요청 payload 파싱 결과입니다. */
     struct ParsedUdpBindRequest {
         std::string session_id;           ///< gateway 내부 세션 ID
         std::uint64_t nonce{0};           ///< bind ticket 논스 값
         std::uint64_t expires_unix_ms{0}; ///< ticket 만료 시각(Epoch ms)
         std::string token;                ///< bind ticket 서명 토큰
     };

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

    /** @brief gateway 세션별 TCP/UDP 바인딩 상태를 보관하는 내부 상태입니다. */
    struct SessionState {
        TransportSessionPtr session;              ///< backend 전송 세션 핸들
        std::string client_id;                     ///< sticky 조회용 클라이언트 ID
        bool udp_bound{false};                     ///< UDP endpoint 바인딩 완료 여부
        boost::asio::ip::udp::endpoint udp_endpoint; ///< 바인딩된 UDP endpoint
        std::uint64_t udp_nonce{0};                ///< 마지막 발급 nonce
        std::uint64_t udp_expires_unix_ms{0};      ///< bind ticket 만료 시각
        std::uint64_t udp_ticket_issued_unix_ms{0}; ///< bind ticket 발급 시각
        std::uint32_t udp_bind_fail_attempts{0};   ///< 최근 bind 실패 누적 횟수(backoff 계산용)
        std::uint64_t udp_bind_retry_after_unix_ms{0}; ///< 다음 bind 재시도 허용 시각(Epoch ms)
        std::string udp_token;                     ///< bind 검증 토큰
        UdpSequencedMetrics udp_sequenced_metrics; ///< UDP 순서/품질 추적기
        bool rudp_selected{false};                 ///< canary/게이트에 의해 RUDP가 선택된 세션 여부
        bool rudp_fallback_to_tcp{false};          ///< RUDP 실패 후 TCP fallback 고정 여부
        std::unique_ptr<server::core::net::rudp::RudpEngine> rudp_engine; ///< 세션별 RUDP 엔진 상태
    };
    std::mutex session_mutex_;
    std::unordered_map<std::string, SessionState> sessions_;

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
    server::core::net::rudp::RudpConfig rudp_config_{};
    UdpBindAbuseGuard udp_bind_abuse_guard_;
    std::unique_ptr<boost::asio::ip::udp::socket> udp_socket_;
    boost::asio::ip::udp::endpoint udp_remote_endpoint_;
    std::array<std::uint8_t, 2048> udp_read_buffer_{};

    // 상태 및 저장소
    std::shared_ptr<server::storage::redis::IRedisClient> redis_client_;
    std::shared_ptr<server::state::IInstanceStateBackend> backend_registry_;
    std::unique_ptr<SessionDirectory> session_directory_;
    std::string redis_uri_;

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
    std::atomic<bool> udp_enabled_{false};
};

} // namespace gateway
