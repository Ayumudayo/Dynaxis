#pragma once

#include <memory>
#include <vector>
#include <cstdint>
#include <string>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include "server/core/protocol/frame.hpp"
#include "server/core/memory/memory_pool.hpp"
#include <functional>
#include <queue>

namespace server::core {

namespace asio = boost::asio;

class Dispatcher;
class BufferManager;
struct SessionOptions;
struct SharedState;

using PacketHeader = server::core::protocol::FrameHeader;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket socket,
            Dispatcher& dispatcher,
            BufferManager& buffer_manager,
            std::shared_ptr<const SessionOptions> options,
            std::shared_ptr<SharedState> state);

    void start();
    void stop();

    // 세션 종료 시 호출될 콜백을 설정한다.
    void set_on_close(std::function<void(std::shared_ptr<Session>)> cb) { on_close_ = std::move(cb); }

    // 표준 에러 프레임 송신
    void send_error(std::uint16_t code, const std::string& msg);

    // 완전한 프레임을 직접 전송(헤더 포함)
    void async_send(BufferManager::PooledBuffer data, size_t frame_size);
    // payload와 msg_id를 받아 프레임을 구성하여 전송
    void async_send(std::uint16_t msg_id, const std::vector<std::uint8_t>& payload, std::uint16_t flags = 0);

    std::uint32_t session_id() const { return session_id_; }

private:
    void do_read_header();
    void do_read_body(std::size_t body_len);
    void do_write();
    std::pair<BufferManager::PooledBuffer, size_t> make_frame(std::uint16_t msg_id,
                                                std::uint16_t flags,
                                                const std::vector<std::uint8_t>& payload,
                                                std::uint32_t seq,
                                                std::uint32_t utc_ts_ms32);
    void send_hello();
    void on_stopped();
    void arm_read_timeout();
    void arm_heartbeat();

public:
    // 클라이언트 원격 IP 문자열 반환(예: "127.0.0.1"). 오류 시 빈 문자열.
    std::string remote_ip() const;

    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    Dispatcher& dispatcher_;
    BufferManager& buffer_manager_;
    PacketHeader header_{};
    BufferManager::PooledBuffer read_buf_;
    std::queue<std::pair<BufferManager::PooledBuffer, size_t>> send_queue_;
    bool writing_{false};
    std::size_t queued_bytes_{0};
    std::shared_ptr<const SessionOptions> options_;
    std::shared_ptr<SharedState> state_;
    bool stopped_{false};
    boost::asio::steady_timer read_timer_{socket_.get_executor()};
    boost::asio::steady_timer heartbeat_timer_{socket_.get_executor()};
    std::uint32_t tx_seq_{1};
    std::function<void(std::shared_ptr<Session>)> on_close_{};
    std::uint32_t session_id_{0};
};
} // namespace server::core
