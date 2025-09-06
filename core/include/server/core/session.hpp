#pragma once

#include <memory>
#include <vector>
#include <cstdint>
#include <string>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include "server/core/protocol/frame.hpp"

namespace server::core {

namespace asio = boost::asio;

class Dispatcher;
struct SessionOptions;
struct SharedState;

using PacketHeader = server::core::protocol::FrameHeader;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket socket,
            Dispatcher& dispatcher,
            std::shared_ptr<const SessionOptions> options,
            std::shared_ptr<SharedState> state);

    void start();
    void stop();

    // 완전한 프레임을 직접 전송(헤더 포함)
    void async_send(std::vector<std::uint8_t> data);
    // payload와 msg_id를 받아 프레임을 구성하여 전송
    void async_send(std::uint16_t msg_id, const std::vector<std::uint8_t>& payload, std::uint16_t flags = 0);

private:
    void do_read_header();
    void do_read_body(std::size_t body_len);
    void do_write();
    static std::vector<std::uint8_t> make_frame(std::uint16_t msg_id,
                                                std::uint16_t flags,
                                                const std::vector<std::uint8_t>& payload,
                                                std::uint32_t seq,
                                                std::uint32_t utc_ts_ms32);
    void send_hello();
    void send_error(std::uint16_t code, const std::string& msg);
    void on_stopped();
    void arm_read_timeout();
    void arm_heartbeat();

    asio::ip::tcp::socket socket_;
    asio::strand<asio::any_io_executor> strand_;
    Dispatcher& dispatcher_;
    PacketHeader header_{};
    std::vector<std::uint8_t> read_buf_;
    std::vector<std::vector<std::uint8_t>> send_queue_;
    bool writing_{false};
    std::size_t queued_bytes_{0};
    std::shared_ptr<const SessionOptions> options_;
    std::shared_ptr<SharedState> state_;
    bool stopped_{false};
    boost::asio::steady_timer read_timer_{socket_.get_executor()};
    boost::asio::steady_timer heartbeat_timer_{socket_.get_executor()};
    std::uint32_t tx_seq_{1};
};

} // namespace server::core
