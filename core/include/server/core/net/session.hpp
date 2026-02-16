#pragma once

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "server/core/memory/memory_pool.hpp"
#include "server/core/protocol/packet.hpp"

namespace server::core {

namespace asio = boost::asio;

class Dispatcher;
class BufferManager;
struct SessionOptions;
struct SharedState;

using PacketHeader = server::core::protocol::PacketHeader;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket socket,
            Dispatcher& dispatcher,
            BufferManager& buffer_manager,
            std::shared_ptr<const SessionOptions> options,
            std::shared_ptr<SharedState> state);

    void start();
    void stop();

    // 세션 종료 직전에 한 번 호출할 콜백을 등록한다.
    void set_on_close(std::function<void(std::shared_ptr<Session>)> cb) { on_close_ = std::move(cb); }

    // MSG_ERR 패킷을 만들어 전송한다.
    void send_error(std::uint16_t code, const std::string& msg);

    // 이미 직렬화된 패킷을 전송 큐에 추가한다.
    void async_send(BufferManager::PooledBuffer data, size_t packet_size);
    // msg_id 와 payload 를 받아 Packet 으로 직렬화한 뒤 전송한다.
    void async_send(std::uint16_t msg_id, const std::vector<std::uint8_t>& payload, std::uint16_t flags = 0);

    std::uint32_t session_id() const { return session_id_; }

private:
    void do_read_header();
    void do_read_body(std::size_t body_len);
    void do_write();
    std::pair<BufferManager::PooledBuffer, size_t> make_packet(std::uint16_t msg_id,
                                                              std::uint16_t flags,
                                                              const std::vector<std::uint8_t>& payload,
                                                              std::uint32_t seq,
                                                              std::uint32_t utc_ts_ms32);
    void send_hello();
    void arm_read_timeout();
    void arm_write_timeout();
    void arm_heartbeat();

public:
    // 클라이언트 원격 IP 를 문자열로 반환한다. 실패하면 빈 문자열을 돌려준다.
    std::string remote_ip() const;

    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    Dispatcher& dispatcher_;
    BufferManager& buffer_manager_;
    PacketHeader header_{};
    BufferManager::PooledBuffer read_buf_;
    std::queue<std::pair<BufferManager::PooledBuffer, size_t>> send_queue_;
    std::size_t queued_bytes_{0};
    std::shared_ptr<const SessionOptions> options_;
    std::shared_ptr<SharedState> state_;
    std::atomic<bool> stopped_{false};
    boost::asio::steady_timer read_timer_{socket_.get_executor()};
    boost::asio::steady_timer write_timer_{socket_.get_executor()};
    boost::asio::steady_timer heartbeat_timer_{socket_.get_executor()};
    std::uint32_t tx_seq_{1};
    std::function<void(std::shared_ptr<Session>)> on_close_{};
    std::uint32_t session_id_{0};
};

} // namespace server::core

