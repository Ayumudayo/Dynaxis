#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <deque>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "gateway/auth/authenticator.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"
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
    /** @brief backend 선택 결과입니다. */
    struct SelectedBackend {
        server::state::InstanceRecord record;
        bool sticky_hit{false};
    };

    /**
     * @brief backend 서버와의 TCP 연결을 관리하는 내부 클래스입니다.
     *
     * `GatewayConnection`(클라이언트)과 game server 사이에서
     * payload를 양방향 브리지합니다.
     */
    class BackendConnection : public std::enable_shared_from_this<BackendConnection> {
    public:
        /**
         * @brief backend 연결을 생성합니다.
         * @param app 상위 GatewayApp 참조
         * @param session_id gateway 내부 세션 ID
         * @param client_id 클라이언트 식별자
         * @param backend_instance_id 선택된 backend 인스턴스 ID
         * @param sticky_hit sticky 라우팅 적중 여부
         * @param connection 클라이언트 연결 weak 포인터
         * @param send_queue_max_bytes backend 송신 큐 최대 바이트
         * @param connect_timeout backend connect timeout
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
         * @brief backend로 TCP connect를 시작합니다.
         * @param host backend host
         * @param port backend port
         */
        void connect(const std::string& host, std::uint16_t port);

        /**
         * @brief backend로 payload를 비동기 전송 큐에 넣습니다.
         * @param payload 전송할 바이트 배열
         */
        void send(std::vector<std::uint8_t> payload);

        /**
         * @brief backend로 raw bytes를 복사해 비동기 전송 큐에 넣습니다.
         * @param data 전송할 바이트 포인터
         * @param length 전송할 바이트 길이
         */
        void send(const std::uint8_t* data, std::size_t length);

        /** @brief backend 연결을 종료합니다. */
        void close();

        /** @brief gateway 내부 세션 ID를 반환합니다. */
        const std::string& session_id() const;

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
        std::array<std::uint8_t, 8192> buffer_;
        std::atomic<bool> closed_{false};
        
        std::mutex send_mutex_;
        std::deque<std::vector<std::uint8_t>> write_queue_;
        std::size_t queued_bytes_{0};
        std::size_t send_queue_max_bytes_{256 * 1024};
        std::chrono::milliseconds connect_timeout_{5000};
        bool connected_{false};
        bool write_in_progress_{false};
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
        connections_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_resolve_fail() {
        backend_resolve_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_connect_fail() {
        backend_connect_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_connect_timeout() {
        backend_connect_timeout_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_write_error() {
        backend_write_error_total_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_backend_send_queue_overflow() {
        backend_send_queue_overflow_total_.fetch_add(1, std::memory_order_relaxed);
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

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    std::shared_ptr<server::core::net::TransportListener> listener_;
    server::core::app::AppHost app_host_{"gateway_app"};
    std::shared_ptr<auth::IAuthenticator> authenticator_;
    std::string gateway_id_;
    std::string listen_host_;
    std::uint16_t listen_port_{6000};

 private:
     void on_backend_connected(const std::string& client_id,
                               const std::string& backend_instance_id,
                               bool sticky_hit);
     void configure_gateway();
     void configure_infrastructure();
     void start_listener();

     void start_infrastructure_probe();
     void stop_infrastructure_probe();

    struct SessionState {
        BackendConnectionPtr session;
    };
    std::mutex session_mutex_;
    std::unordered_map<std::string, SessionState> sessions_;

    std::atomic<std::uint64_t> connections_total_{0};
    std::atomic<std::uint64_t> backend_resolve_fail_total_{0};
    std::atomic<std::uint64_t> backend_connect_fail_total_{0};
    std::atomic<std::uint64_t> backend_connect_timeout_total_{0};
    std::atomic<std::uint64_t> backend_write_error_total_{0};
    std::atomic<std::uint64_t> backend_send_queue_overflow_total_{0};

    std::string boot_id_;
    std::uint16_t metrics_port_{6001};
    std::uint32_t backend_connect_timeout_ms_{5000};
    std::size_t backend_send_queue_max_bytes_{256 * 1024};

     // State & Storage
     std::shared_ptr<server::storage::redis::IRedisClient> redis_client_;
     std::shared_ptr<server::state::IInstanceStateBackend> backend_registry_;
     std::unique_ptr<SessionDirectory> session_directory_;
     std::string redis_uri_;

     std::atomic<bool> infra_probe_stop_{false};
     std::thread infra_probe_thread_;
  };

 } // namespace gateway
