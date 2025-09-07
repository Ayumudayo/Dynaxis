#include "server/core/session.hpp"
#include "server/core/dispatcher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/options.hpp"
#include "server/core/shared_state.hpp"
#include "server/core/protocol_flags.hpp"
#include "server/core/protocol_errors.hpp"

#include <array>
#include <cstring>
#include <algorithm>
#include <chrono>
#include "server/core/protocol/frame.hpp"

using boost::system::error_code;

namespace server::core {

namespace { }

Session::Session(asio::ip::tcp::socket socket,
                 Dispatcher& dispatcher,
                 std::shared_ptr<const SessionOptions> options,
                 std::shared_ptr<SharedState> state)
    : socket_(std::move(socket))
    , strand_(socket_.get_executor())
    , dispatcher_(dispatcher)
    , options_(std::move(options))
    , state_(std::move(state))
    , read_timer_(socket_.get_executor())
    , heartbeat_timer_(socket_.get_executor()) {
    if (state_) state_->connection_count.fetch_add(1);
    if (state_) session_id_ = state_->next_session_id.fetch_add(1);
}

void Session::start() {
    send_hello();
    do_read_header();
    arm_read_timeout();
    arm_heartbeat();
}

void Session::stop() {
    if (stopped_) return;
    stopped_ = true;
    error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    read_timer_.cancel();
    heartbeat_timer_.cancel();
    if (state_) state_->connection_count.fetch_sub(1);
    log::info("세션 종료: " + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    if (on_close_) {
        try { on_close_(*this); } catch (...) {}
    }
}

void Session::async_send(std::vector<std::uint8_t> data) {
    // data는 이미 헤더가 포함된 완전한 프레임으로 간주한다.
    asio::dispatch(strand_, [self = shared_from_this(), data = std::move(data)]() mutable {
        if (self->stopped_) return;
        std::size_t frame_size = data.size();
        if (self->options_ && self->options_->send_queue_max > 0 && self->queued_bytes_ + frame_size > self->options_->send_queue_max) {
            log::warn("송신 큐 상한 초과로 세션 종료");
            self->stop();
            return;
        }
        bool kick_write = false;
        if (!self->writing_) {
            self->writing_ = true;
            kick_write = true;
        }
        self->queued_bytes_ += frame_size;
        self->send_queue_.emplace_back(std::move(data));
        if (kick_write) self->do_write();
    });
}

void Session::async_send(std::uint16_t msg_id, const std::vector<std::uint8_t>& payload, std::uint16_t flags) {
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    auto frame = make_frame(msg_id, flags, payload, tx_seq_++, now32);
    async_send(std::move(frame));
}

void Session::do_read_header() {
    auto self = shared_from_this();
    read_buf_.resize(server::core::protocol::k_header_bytes);
    asio::async_read(socket_, asio::buffer(read_buf_),
        asio::bind_executor(strand_, [this, self](const error_code& ec, std::size_t n) {
            if (ec) {
                log::debug(std::string("헤더 읽기 실패: ") + ec.message());
                stop();
                return;
            }
            if (n != server::core::protocol::k_header_bytes) {
                log::warn("헤더 길이 불일치");
                stop();
                return;
            }
            server::core::protocol::decode_header(read_buf_.data(), header_);
            if (options_ && options_->recv_max_payload > 0 && header_.length > options_->recv_max_payload) {
                send_error(server::core::protocol::errc::LENGTH_LIMIT_EXCEEDED, "payload too large");
                stop();
                return;
            }
            // 수신 성공 → read 타이머 재무장
            arm_read_timeout();
            do_read_body(header_.length);
        }));
}

std::vector<std::uint8_t> Session::make_frame(std::uint16_t msg_id,
                                              std::uint16_t flags,
                                              const std::vector<std::uint8_t>& payload,
                                              std::uint32_t seq,
                                              std::uint32_t utc_ts_ms32) {
    std::vector<std::uint8_t> out;
    out.resize(server::core::protocol::k_header_bytes + payload.size());
    server::core::protocol::FrameHeader h{static_cast<std::uint16_t>(payload.size()), msg_id, flags, seq, utc_ts_ms32};
    server::core::protocol::encode_header(h, out.data());
    if (!payload.empty()) { std::memcpy(out.data() + server::core::protocol::k_header_bytes, payload.data(), payload.size()); }
    return out;
}

void Session::do_read_body(std::size_t body_len) {
    auto self = shared_from_this();
    read_buf_.resize(body_len);
    if (body_len == 0) {
        // 빈 페이로드도 허용. 즉시 디스패치 시도.
        dispatcher_.dispatch(header_.msg_id, *this, std::span<const std::uint8_t>{});
        do_read_header();
        return;
    }
    asio::async_read(socket_, asio::buffer(read_buf_),
        asio::bind_executor(strand_, [this, self](const error_code& ec, std::size_t n) {
            if (ec) {
                log::debug(std::string("본문 읽기 실패: ") + ec.message());
                stop();
                return;
            }
            if (n != read_buf_.size()) {
                log::warn("본문 길이 불일치");
                stop();
                return;
            }
            // 수신 성공 → read 타이머 재무장
            arm_read_timeout();
            if (!dispatcher_.dispatch(header_.msg_id, *this, std::span<const std::uint8_t>(read_buf_.data(), read_buf_.size()))) {
                send_error(server::core::protocol::errc::UNKNOWN_MSG_ID, "unknown msg");
            }
            do_read_header();
        }));
}

void Session::do_write() {
    if (send_queue_.empty()) {
        writing_ = false;
        return;
    }
    auto self = shared_from_this();
    auto& front = send_queue_.front();
    asio::async_write(socket_, asio::buffer(front),
        asio::bind_executor(strand_, [this, self](const error_code& ec, std::size_t /*n*/) {
            if (ec) {
                log::debug(std::string("쓰기 실패: ") + ec.message());
                stop();
                return;
            }
            self->queued_bytes_ -= self->send_queue_.front().size();
            send_queue_.erase(send_queue_.begin());
            do_write();
        }));
}

void Session::send_hello() {
    std::vector<std::uint8_t> payload;
    // HELLO payload(v1.1): proto_major(2) + proto_minor(2) + capabilities(2) + hb_x10ms(2) + epoch_high32(4) = 12 bytes
    payload.resize(12);
    server::core::protocol::write_be16(1, payload.data()); // proto_major
    server::core::protocol::write_be16(1, payload.data() + 2); // proto_minor (헤더 v1.1)
    // capabilities: 압축 지원 + sender_sid 포함
    std::uint16_t caps = static_cast<std::uint16_t>(server::core::protocol::CAP_COMPRESS_SUPP | server::core::protocol::CAP_SENDER_SID);
    server::core::protocol::write_be16(caps, payload.data() + 4);
    unsigned hb = options_ ? options_->heartbeat_interval_ms / 10 : 0;
    server::core::protocol::write_be16(static_cast<std::uint16_t>(hb), payload.data() + 6);
    // epoch_high32: 서버 UTC ms 상위 32비트 제공(클라가 64비트 재구성 가능)
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t epoch_high32 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(now64) >> 32) & 0xFFFFFFFFu);
    server::core::protocol::write_be32(epoch_high32, payload.data() + 8);
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    auto frame = make_frame(0x0001 /*MSG_HELLO*/, 0, payload, tx_seq_++, now32);
    async_send(std::move(frame));
}

void Session::send_error(std::uint16_t code, const std::string& msg) {
    std::vector<std::uint8_t> payload;
    std::uint16_t len = static_cast<std::uint16_t>(std::min<std::size_t>(msg.size(), 400));
    payload.resize(4 + len);
    server::core::protocol::write_be16(code, payload.data());
    server::core::protocol::write_be16(len, payload.data() + 2);
    if (len) std::memcpy(payload.data() + 4, msg.data(), len);
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    auto frame = make_frame(0x0004 /*MSG_ERR*/, 0, payload, tx_seq_++, now32);
    async_send(std::move(frame));
}

void Session::arm_read_timeout() {
    if (!options_ || options_->read_timeout_ms == 0) return;
    auto self = shared_from_this();
    asio::dispatch(strand_, [this, self]() {
        // 기존 타이머 취소 후 재무장
        read_timer_.cancel();
        read_timer_.expires_after(std::chrono::milliseconds(options_->read_timeout_ms));
        read_timer_.async_wait(asio::bind_executor(strand_, [this, self](const error_code& ec){
            if (ec || stopped_) return; // cancel 등 무시
            log::warn("read timeout");
            stop();
        }));
    });
}

void Session::arm_heartbeat() {
    if (!options_ || options_->heartbeat_interval_ms == 0) return;
    heartbeat_timer_.expires_after(std::chrono::milliseconds(options_->heartbeat_interval_ms));
    auto self = shared_from_this();
    heartbeat_timer_.async_wait(asio::bind_executor(strand_, [this, self](const error_code& ec){
        if (ec || stopped_) return;
        // 서버 주도 핑은 선택. 현재는 타이머 유지만 수행.
        arm_heartbeat();
    }));
}

} // namespace server::core
