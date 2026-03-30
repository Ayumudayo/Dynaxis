#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "gateway/transport_session.hpp"

namespace gateway {

class GatewayApp;
class GatewayConnection;

/**
 * @brief 선택된 backend 서버와의 TCP 연결을 관리하는 내부 브리지 구현입니다.
 *
 * public gateway surface에는 노출하지 않고, `GatewayApp` 내부 orchestration과
 * `GatewayConnection` 사이의 bridge 세부 동작만 담당합니다.
 */
class BackendConnection final : public ITransportSession,
                                public std::enable_shared_from_this<BackendConnection> {
public:
    BackendConnection(GatewayApp& app,
                      std::string session_id,
                      std::string client_id,
                      std::string backend_instance_id,
                      bool sticky_hit,
                      std::weak_ptr<GatewayConnection> connection,
                      std::size_t send_queue_max_bytes,
                      std::chrono::milliseconds connect_timeout);
    ~BackendConnection() override;

    void connect(const std::string& host, std::uint16_t port);
    void send(std::vector<std::uint8_t> payload) override;
    void send(const std::uint8_t* data, std::size_t length) override;
    void close() override;
    const std::string& session_id() const override;
    const std::string& backend_instance_id() const noexcept { return backend_instance_id_; }

private:
    void do_connect(const std::string& host, std::uint16_t port);
    void do_read();
    void on_read(const boost::system::error_code& ec, std::size_t bytes_transferred);
    void do_write();
    void on_connect_timeout();
    bool schedule_connect_retry(const char* reason);
    void close_socket_for_retry();

    GatewayApp& app_;
    std::string session_id_;
    std::string client_id_;
    std::string backend_instance_id_;
    bool sticky_hit_{false};
    std::weak_ptr<GatewayConnection> connection_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer connect_timer_;
    boost::asio::steady_timer retry_timer_;
    std::array<std::uint8_t, 8192> buffer_{};
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
};

using BackendConnectionPtr = std::shared_ptr<BackendConnection>;

} // namespace gateway
