#include "server/core/net/session.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/config/options.hpp"
#include "server/core/state/shared_state.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/runtime_metrics.hpp"

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
                 BufferManager& buffer_manager,
                 std::shared_ptr<const SessionOptions> options,
                 std::shared_ptr<SharedState> state)
    : socket_(std::move(socket))
    , strand_(socket_.get_executor())
    , dispatcher_(dispatcher)
    , buffer_manager_(buffer_manager)
    , options_(std::move(options))
    , state_(std::move(state))
    , read_timer_(socket_.get_executor())
    , heartbeat_timer_(socket_.get_executor()) {
    if (state_) state_->connection_count.fetch_add(1);
    if (state_) session_id_ = state_->next_session_id.fetch_add(1);
    runtime_metrics::record_session_start();
}

std::string Session::remote_ip() const {
    try {
        auto ep = socket_.remote_endpoint();
        return ep.address().to_string();
    } catch (...) {
        return std::string();
    }
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
    runtime_metrics::record_session_stop();
    log::info("Session closed: " + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    if (on_close_) {
        try { on_close_(shared_from_this()); } catch (...) {}
    }
}

void Session::async_send(BufferManager::PooledBuffer data, size_t frame_size) {
    // data는 풀에서 가져온 연속 버퍼이므로 그대로 큐에 넣는다.
    asio::dispatch(strand_, [self = shared_from_this(), data = std::move(data), frame_size]() mutable {
        if (self->stopped_) return;
        if (self->options_ && self->options_->send_queue_max > 0 && self->queued_bytes_ + frame_size > self->options_->send_queue_max) {
            runtime_metrics::record_send_queue_drop();
            log::warn("Send queue limit exceeded; stopping session");
            self->stop();
            return;
        }
        bool kick_write = self->send_queue_.empty();
        self->queued_bytes_ += frame_size;
        self->send_queue_.push({std::move(data), frame_size});
        if (kick_write) {
            self->do_write();
        }
    });
}

void Session::async_send(std::uint16_t msg_id, const std::vector<std::uint8_t>& payload, std::uint16_t flags) {
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    
    auto [buffer, frame_size] = make_frame(msg_id, flags, payload, tx_seq_++, now32);
    if (buffer) {
        async_send(std::move(buffer), frame_size);
    }
}

std::pair<BufferManager::PooledBuffer, size_t> Session::make_frame(std::uint16_t msg_id,
                                              std::uint16_t flags,
                                              const std::vector<std::uint8_t>& payload,
                                              std::uint32_t seq,
                                              std::uint32_t utc_ts_ms32) {
    size_t frame_size = server::core::protocol::k_header_bytes + payload.size();
    if (frame_size > buffer_manager_.GetBlockSize()) {
        log::error("Outgoing frame exceeds memory pool block size.");
        return {nullptr, 0};
    }

    auto buffer = buffer_manager_.Acquire();
    if (!buffer) {
        log::error("Memory pool allocation failed.");
        return {nullptr, 0};
    }

    server::core::protocol::FrameHeader h{static_cast<std::uint16_t>(payload.size()), msg_id, flags, seq, utc_ts_ms32};
    server::core::protocol::encode_header(h, reinterpret_cast<std::uint8_t*>(buffer.get()));
    if (!payload.empty()) { 
        std::memcpy(reinterpret_cast<std::uint8_t*>(buffer.get()) + server::core::protocol::k_header_bytes, payload.data(), payload.size()); 
    }
    return {std::move(buffer), frame_size};
}


void Session::do_read_header() {
    auto self = shared_from_this();
    read_buf_ = buffer_manager_.Acquire();
    if (!read_buf_) {
        runtime_metrics::record_frame_error();
        log::error("Failed to acquire read buffer.");
        stop();
        return;
    }
    asio::async_read(socket_, asio::buffer(read_buf_.get(), server::core::protocol::k_header_bytes),
        asio::bind_executor(strand_, [this, self](const error_code& ec, std::size_t n) {
            if (ec) {
                runtime_metrics::record_frame_error();
                log::debug(std::string("Failed to read header: ") + ec.message());
                stop();
                return;
            }
            if (n != server::core::protocol::k_header_bytes) {
                runtime_metrics::record_frame_error();
                log::warn("Header size mismatch");
                stop();
                return;
            }
            server::core::protocol::decode_header(reinterpret_cast<const std::uint8_t*>(read_buf_.get()), header_);
            if (options_ && options_->recv_max_payload > 0 && header_.length > options_->recv_max_payload) {
                runtime_metrics::record_frame_error();
                send_error(server::core::protocol::errc::LENGTH_LIMIT_EXCEEDED, "payload too large");
                stop();
                return;
            }
            // 헤더를 읽었으니 읽기 타임아웃을 다시 설정한다.
            arm_read_timeout();
            do_read_body(header_.length);
        }));
}

void Session::do_read_body(std::size_t body_len) {
    auto self = shared_from_this();
    if (body_len == 0) {
        // payload가 비어 있으면 빈 span을 그대로 전달한다.
        runtime_metrics::record_frame_payload(0);
        runtime_metrics::record_frame_ok();
        auto start = std::chrono::steady_clock::now();
        bool handled = dispatcher_.dispatch(header_.msg_id, *this, std::span<const std::uint8_t>{});
        runtime_metrics::record_dispatch_opcode(header_.msg_id);
        auto elapsed = std::chrono::steady_clock::now() - start;
        runtime_metrics::record_dispatch_attempt(handled, elapsed);
        if (!handled) {
            send_error(server::core::protocol::errc::UNKNOWN_MSG_ID, "unknown msg");
        }
        do_read_header();
        return;
    }
    if (body_len > buffer_manager_.GetBlockSize()) {
        runtime_metrics::record_frame_error();
        log::error("Body is larger than memory pool block size.");
        stop();
        return;
    }
    read_buf_ = buffer_manager_.Acquire();
    if (!read_buf_) {
        runtime_metrics::record_frame_error();
        log::error("Failed to acquire body buffer.");
        stop();
        return;
    }
    asio::async_read(socket_, asio::buffer(read_buf_.get(), body_len),
        asio::bind_executor(strand_, [this, self](const error_code& ec, std::size_t n) {
            if (ec) {
                runtime_metrics::record_frame_error();
                log::debug(std::string("Failed to read body: ") + ec.message());
                stop();
                return;
            }
            if (n != header_.length) {
                runtime_metrics::record_frame_error();
                log::warn("Body size mismatch");
                stop();
                return;
            }
            // 본문까지 읽었으니 읽기 타임아웃을 재설정한다.
            arm_read_timeout();
            runtime_metrics::record_frame_payload(header_.length);
            runtime_metrics::record_frame_ok();
            auto start = std::chrono::steady_clock::now();
            bool handled = dispatcher_.dispatch(header_.msg_id, *this, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(read_buf_.get()), header_.length));
            runtime_metrics::record_dispatch_opcode(header_.msg_id);
            auto elapsed = std::chrono::steady_clock::now() - start;
            runtime_metrics::record_dispatch_attempt(handled, elapsed);
            if (!handled) {
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
    auto& front_pair = send_queue_.front();
    const auto frame_size = front_pair.second;
    asio::async_write(socket_, asio::buffer(front_pair.first.get(), frame_size),
        asio::bind_executor(strand_, [this, self, frame_size](const error_code& ec, std::size_t /*n*/) {
            if (ec) {
                log::debug(std::string("Write failed: ") + ec.message());
                stop();
                return;
            }
            if (self->queued_bytes_ >= frame_size) {
                self->queued_bytes_ -= frame_size;
            } else {
                self->queued_bytes_ = 0;
            }
            self->send_queue_.pop(); // PooledBuffer will be released here
            self->do_write();
        }));
}

void Session::send_hello() {
    std::vector<std::uint8_t> payload_vec;
    payload_vec.resize(12);
    server::core::protocol::write_be16(1, payload_vec.data()); // proto_major
    server::core::protocol::write_be16(1, payload_vec.data() + 2); // proto_minor (헤더 v1.1)
    // capabilities: 클라이언트에게 sender_sid 지원 여부를 알린다.
    std::uint16_t caps = static_cast<std::uint16_t>(server::core::protocol::CAP_COMPRESS_SUPP | server::core::protocol::CAP_SENDER_SID);
    server::core::protocol::write_be16(caps, payload_vec.data() + 4);
    unsigned hb = options_ ? options_->heartbeat_interval_ms / 10 : 0;
    server::core::protocol::write_be16(static_cast<std::uint16_t>(hb), payload_vec.data() + 6);
    // epoch_high32: 클라이언트가 64비트 UTC 타임스탬프를 복원할 수 있도록 상위 32비트를 전달한다.
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t epoch_high32 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(now64) >> 32) & 0xFFFFFFFFu);
    server::core::protocol::write_be32(epoch_high32, payload_vec.data() + 8);
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);

    auto [buffer, frame_size] = make_frame(0x0001 /*MSG_HELLO*/, 0, payload_vec, tx_seq_++, now32);
    if (buffer) {
        async_send(std::move(buffer), frame_size);
    }
}

void Session::send_error(std::uint16_t code, const std::string& msg) {
    std::vector<std::uint8_t> payload_vec;
    std::uint16_t len = static_cast<std::uint16_t>(std::min<std::size_t>(msg.size(), 400));
    payload_vec.resize(4 + len);
    server::core::protocol::write_be16(code, payload_vec.data());
    server::core::protocol::write_be16(len, payload_vec.data() + 2);
    if (len) std::memcpy(payload_vec.data() + 4, msg.data(), len);
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    
    auto [buffer, frame_size] = make_frame(0x0004 /*MSG_ERR*/, 0, payload_vec, tx_seq_++, now32);
    if (buffer) {
        async_send(std::move(buffer), frame_size);
    }
}

void Session::arm_read_timeout() {
    if (!options_ || options_->read_timeout_ms == 0) return;
    auto self = shared_from_this();
    asio::dispatch(strand_, [this, self]() {
        // 읽기 타이머를 초기화하고 다시 설정한다.
        read_timer_.cancel();
        read_timer_.expires_after(std::chrono::milliseconds(options_->read_timeout_ms));
        read_timer_.async_wait(asio::bind_executor(strand_, [this, self](const error_code& ec) {
            if (ec || stopped_) return; // 취소되었거나 세션이 종료된 경우 무시한다.
            runtime_metrics::record_session_timeout();
            if (options_ && options_->heartbeat_interval_ms > 0) {
                runtime_metrics::record_heartbeat_timeout();
            }
            log::warn("read timeout");
            stop();
        }));
    });
}

void Session::arm_heartbeat() {
    if (!options_ || options_->heartbeat_interval_ms == 0) return;
    auto self = shared_from_this();
    asio::dispatch(strand_, [this, self]() {
        // heartbeat 주기를 유지하기 위해 타이머를 다시 설정한다.
        heartbeat_timer_.cancel();
        heartbeat_timer_.expires_after(std::chrono::milliseconds(options_->heartbeat_interval_ms));
        heartbeat_timer_.async_wait(asio::bind_executor(strand_, [this, self](const error_code& ec) {
            if (ec || stopped_) return; // 타이머가 취소되었거나 세션이 이미 종료된 경우 무시한다.
            arm_heartbeat();
        }));
    });
}

} // namespace server::core

