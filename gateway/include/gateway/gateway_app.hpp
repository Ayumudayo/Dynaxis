#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <deque>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "gateway/auth/authenticator.hpp"
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"
#include "server/core/metrics/http_server.hpp"
#include "server/state/instance_registry.hpp"
#include "server/storage/redis/client.hpp"
#include "gateway/session_directory.hpp"

namespace gateway {

class GatewayConnection;

// Gateway 애플리케이션의 메인 클래스입니다.
// TCP 리스너를 구동하여 클라이언트 연결을 수락하고,
// 각 연결에 대해 로드 밸런서(LB)와의 gRPC 세션을 관리합니다.
class GatewayApp {
public:
    // 서버와의 TCP 연결 세션을 관리하는 내부 클래스입니다.
    // GatewayConnection(Client)과 Game Server 간의 트래픽을 중계합니다.
    class BackendSession : public std::enable_shared_from_this<BackendSession> {
    public:
        BackendSession(GatewayApp& app,
                       std::string session_id,
                       std::string client_id,
                       std::weak_ptr<GatewayConnection> connection);
        ~BackendSession();

        void connect(const std::string& host, std::uint16_t port);
        void send(const std::vector<std::uint8_t>& payload);
        void close();
        const std::string& session_id() const;

    private:
        void do_connect(const std::string& host, std::uint16_t port);
        void do_read();
        void on_read(const boost::system::error_code& ec, std::size_t bytes_transferred);
        void do_write();

        GatewayApp& app_;
        std::string session_id_;
        std::string client_id_;
        std::weak_ptr<GatewayConnection> connection_;
        boost::asio::ip::tcp::socket socket_;
        std::array<std::uint8_t, 8192> buffer_;
        std::atomic<bool> closed_{false};
        
        std::mutex send_mutex_;
        std::deque<std::vector<std::uint8_t>> write_queue_;
        bool connected_{false};
        bool write_in_progress_{false};
    };
    using BackendSessionPtr = std::shared_ptr<BackendSession>;

    GatewayApp();
    ~GatewayApp();

    int run();
    void stop();

    BackendSessionPtr create_backend_session(const std::string& client_id,
                                             std::weak_ptr<GatewayConnection> connection);
    void close_backend_session(const std::string& session_id);
    
    // Redis Registry에서 최적의 서버를 선택합니다.
    std::optional<std::pair<std::string, std::uint16_t>> select_best_server(const std::string& client_id = "");

    std::string gateway_id() const { return gateway_id_; }

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    std::shared_ptr<server::core::net::Listener> listener_;
    boost::asio::signal_set signals_;
    std::shared_ptr<auth::IAuthenticator> authenticator_;
    std::string gateway_id_;
    std::string listen_host_;
    std::uint16_t listen_port_{6000};

private:
    void configure_gateway();
    void configure_infrastructure();
    void start_listener();
    void handle_signals();

    struct SessionState {
        BackendSessionPtr session;
    };
    std::mutex session_mutex_;
    std::unordered_map<std::string, SessionState> sessions_;
    
    std::unique_ptr<server::core::metrics::MetricsHttpServer> metrics_server_;
    std::uint16_t metrics_port_{6001};

    // State & Storage
    std::shared_ptr<server::storage::redis::IRedisClient> redis_client_;
    std::shared_ptr<server::state::IInstanceStateBackend> backend_registry_;
    std::unique_ptr<SessionDirectory> session_directory_;
    std::string redis_uri_;
};

} // namespace gateway
