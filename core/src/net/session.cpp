#include "server/core/net/session.hpp"
#include "server/core/net/connection_runtime_state.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/config/options.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/version.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/trace/context.hpp"

#include <cstring>
#include <algorithm>
#include <chrono>
using boost::system::error_code;

/**
 * @brief Session의 패킷 IO/타임아웃/heartbeat 구현입니다.
 *
 * 단일 세션에서 발생한 오류를 해당 연결 안에 격리하고,
 * strand 기반 직렬화로 read/write 경합을 피하면서 안정적인 상태 전이를 보장합니다.
 */
namespace server::core {

// Session은 단일 TCP 연결을 비동기적으로 관리하는 핵심 객체입니다.
// 모든 작업이 strand 위에서 진행되어 송수신 경합이 발생하지 않으며,
// SharedState는 총 연결 수와 session_id를 추적해 관측 지표를 제공합니다.
Session::Session(asio::ip::tcp::socket socket,
                 Dispatcher& dispatcher,
                 BufferManager& buffer_manager,
                 std::shared_ptr<const SessionOptions> options,
                 std::shared_ptr<net::ConnectionRuntimeState> state)
    : socket_(std::move(socket))
    , strand_(socket_.get_executor())
    , dispatcher_(dispatcher)
    , buffer_manager_(buffer_manager)
    , options_(std::move(options))
    , state_(std::move(state))
    , read_timer_(socket_.get_executor())
    , write_timer_(socket_.get_executor())
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
    // 서버와 클라이언트의 버전/옵션을 맞추기 위해 HELLO를 가장 먼저 보냅니다.
    // 이후 read/ping 타이머를 동시에 설정해 두면 I/O 경합 없이 진행됩니다.
    send_hello();
    do_read_header();
    arm_read_timeout();
    arm_heartbeat();
}

void Session::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    auto self = shared_from_this();
    asio::dispatch(strand_, [this, self]() {
        if (socket_.is_open()) {
            try {
                socket_.shutdown(asio::ip::tcp::socket::shutdown_both);
            } catch (...) {
            }
            try {
                socket_.close();
            } catch (...) {
            }
        }
        (void)read_timer_.cancel();
        (void)write_timer_.cancel();
        (void)heartbeat_timer_.cancel();
        if (state_) {
            state_->connection_count.fetch_sub(1);
        }
        runtime_metrics::record_session_stop();
        log::info("Session closed: " + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        if (on_close_) {
            try {
                on_close_(self);
            } catch (...) {
            }
        }
    });
}

bool Session::post_serialized(std::function<void()> fn) {
    if (!fn) {
        return false;
    }

    std::shared_ptr<Session> self;
    try {
        self = shared_from_this();
    } catch (...) {
        return false;
    }

    asio::post(strand_, [self, fn = std::move(fn)]() mutable {
        if (self->stopped_.load(std::memory_order_acquire)) {
            return;
        }
        fn();
    });
    return true;
}

void Session::async_send(BufferManager::PooledBuffer data, size_t packet_size) {
    asio::dispatch(strand_, [self = shared_from_this(), data = std::move(data), packet_size]() mutable {
        if (self->stopped_.load(std::memory_order_acquire)) return;
        if (self->options_ && self->options_->send_queue_max > 0 && self->queued_bytes_ + packet_size > self->options_->send_queue_max) {
            runtime_metrics::record_send_queue_drop();
            log::warn("Send queue limit exceeded; stopping session");
            self->stop();
            return;
        }
        bool kick_write = self->send_queue_.empty();
        self->queued_bytes_ += packet_size;
        self->send_queue_.push({std::move(data), packet_size});
        if (kick_write) {
            self->do_write();
        }
    });
}

void Session::async_send(std::uint16_t msg_id, const std::vector<std::uint8_t>& payload, std::uint16_t flags) {
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    
    auto [buffer, packet_size] = make_packet(msg_id, flags, payload, tx_seq_++, now32);
    if (buffer) {
        async_send(std::move(buffer), packet_size);
    }
}

std::pair<BufferManager::PooledBuffer, size_t> Session::make_packet(std::uint16_t msg_id,
                                              std::uint16_t flags,
                                              const std::vector<std::uint8_t>& payload,
                                              std::uint32_t seq,
                                              std::uint32_t utc_ts_ms32) {
    size_t packet_size = server::core::protocol::k_header_bytes + payload.size();
    if (packet_size > buffer_manager_.GetBlockSize()) {
        log::error("Outgoing packet exceeds memory pool block size.");
        return {nullptr, 0};
    }

    auto buffer = buffer_manager_.Acquire();
    if (!buffer) {
        log::error("Memory pool allocation failed.");
        return {nullptr, 0};
    }

    server::core::protocol::PacketHeader h{static_cast<std::uint16_t>(payload.size()), msg_id, flags, seq, utc_ts_ms32};
    server::core::protocol::encode_header(h, reinterpret_cast<std::uint8_t*>(buffer.get()));
    if (!payload.empty()) { 
        std::memcpy(reinterpret_cast<std::uint8_t*>(buffer.get()) + server::core::protocol::k_header_bytes, payload.data(), payload.size()); 
    }
    return {std::move(buffer), packet_size};
}

void Session::do_read_header() {
    auto self = shared_from_this();
    read_buf_ = buffer_manager_.Acquire();
    if (!read_buf_) {
        runtime_metrics::record_packet_error();
        log::error("Failed to acquire read buffer.");
        stop();
        return;
    }
    asio::async_read(socket_, asio::buffer(read_buf_.get(), server::core::protocol::k_header_bytes),
        asio::bind_executor(strand_, [this, self](const error_code& ec, std::size_t n) {
            if (ec) {
                runtime_metrics::record_packet_error();
                log::debug(std::string("Failed to read header: ") + ec.message());
                // 헤더 읽기 실패는 프로토콜 위반이거나 연결 끊김(EOF)이므로 세션을 종료합니다.
                // 4바이트 헤더조차 읽지 못했다면 더 이상 진행할 수 없습니다.
                stop();
                return;
            }
            if (n != server::core::protocol::k_header_bytes) {
                runtime_metrics::record_packet_error();
                log::warn("Header size mismatch");
                stop();
                return;
            }
            server::core::protocol::decode_header(reinterpret_cast<const std::uint8_t*>(read_buf_.get()), header_);
            if (options_ && options_->recv_max_payload > 0 && header_.length > options_->recv_max_payload) {
                runtime_metrics::record_packet_error();
                send_error(server::core::protocol::errc::LENGTH_LIMIT_EXCEEDED, "payload too large");
                stop();
                return;
            }
            arm_read_timeout();
            do_read_body(header_.length);
        }));
}

void Session::do_read_body(std::size_t body_len) {
    auto self = shared_from_this();
    if (body_len == 0) {
        // payload가 비어 있으면 빈 span을 그대로 전달한다.
        runtime_metrics::record_packet_payload(0);
        runtime_metrics::record_packet_ok();

        const std::uint64_t trace_seed = (static_cast<std::uint64_t>(session_id_) << 32)
                                       ^ (static_cast<std::uint64_t>(header_.seq) << 8)
                                       ^ static_cast<std::uint64_t>(header_.msg_id);
        const bool sampled = trace::should_sample(trace_seed);
        trace::ScopedContext trace_scope(
            sampled ? trace::make_trace_id() : std::string{},
            sampled ? trace::make_correlation_id(session_id_, header_.msg_id, header_.seq) : std::string{},
            sampled
        );

        if (trace_scope.active()) {
            log::debug("span_start component=session span=ingress");
            log::debug("span_start component=dispatcher span=dispatch");
        }

        auto start = std::chrono::steady_clock::now();
        bool handled = dispatcher_.dispatch(header_.msg_id, *this, std::span<const std::uint8_t>{});
        runtime_metrics::record_dispatch_opcode(header_.msg_id);
        auto elapsed = std::chrono::steady_clock::now() - start;
        runtime_metrics::record_dispatch_attempt(handled, elapsed);

        if (trace_scope.active()) {
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            log::debug(
                "span_end component=dispatcher span=dispatch duration_us="
                + std::to_string(elapsed_us)
                + " handled=" + (handled ? std::string("true") : std::string("false"))
            );
            log::debug("span_end component=session span=ingress");
        }

        if (!handled) {
            send_error(server::core::protocol::errc::UNKNOWN_MSG_ID, "unknown msg");
        }
        do_read_header();
        return;
    }
    if (body_len > buffer_manager_.GetBlockSize()) {
        runtime_metrics::record_packet_error();
        log::error("Body is larger than memory pool block size.");
        stop();
        return;
    }
    read_buf_ = buffer_manager_.Acquire();
    if (!read_buf_) {
        runtime_metrics::record_packet_error();
        log::error("Failed to acquire body buffer.");
        stop();
        return;
    }
    asio::async_read(socket_, asio::buffer(read_buf_.get(), body_len),
        asio::bind_executor(strand_, [this, self](const error_code& ec, std::size_t n) {
            if (ec) {
                runtime_metrics::record_packet_error();
                log::debug(std::string("Failed to read body: ") + ec.message());
                stop();
                return;
            }
            if (n != header_.length) {
                runtime_metrics::record_packet_error();
                log::warn("Body size mismatch");
                stop();
                return;
            }
            // 본문까지 읽었으니 읽기 타임아웃을 재설정한다.
            arm_read_timeout();
            runtime_metrics::record_packet_payload(header_.length);
            runtime_metrics::record_packet_ok();

            const std::uint64_t trace_seed = (static_cast<std::uint64_t>(session_id_) << 32)
                                           ^ (static_cast<std::uint64_t>(header_.seq) << 8)
                                           ^ static_cast<std::uint64_t>(header_.msg_id);
            const bool sampled = trace::should_sample(trace_seed);
            trace::ScopedContext trace_scope(
                sampled ? trace::make_trace_id() : std::string{},
                sampled ? trace::make_correlation_id(session_id_, header_.msg_id, header_.seq) : std::string{},
                sampled
            );

            if (trace_scope.active()) {
                log::debug("span_start component=session span=ingress");
                log::debug("span_start component=dispatcher span=dispatch");
            }

            auto start = std::chrono::steady_clock::now();
            bool handled = dispatcher_.dispatch(header_.msg_id, *this, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(read_buf_.get()), header_.length));
            runtime_metrics::record_dispatch_opcode(header_.msg_id);
            auto elapsed = std::chrono::steady_clock::now() - start;
            runtime_metrics::record_dispatch_attempt(handled, elapsed);

            if (trace_scope.active()) {
                const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
                log::debug(
                    "span_end component=dispatcher span=dispatch duration_us="
                    + std::to_string(elapsed_us)
                    + " handled=" + (handled ? std::string("true") : std::string("false"))
                );
                log::debug("span_end component=session span=ingress");
            }

            if (!handled) {
                send_error(server::core::protocol::errc::UNKNOWN_MSG_ID, "unknown msg");
            }
            do_read_header();
        }));
}

void Session::do_write() {
    if (send_queue_.empty()) {
        (void)write_timer_.cancel();
        return;
    }
    auto self = shared_from_this();
    auto& front_pair = send_queue_.front();
    const auto packet_size = front_pair.second;
    arm_write_timeout();
    asio::async_write(socket_, asio::buffer(front_pair.first.get(), packet_size),
        asio::bind_executor(strand_, [this, self, packet_size](const error_code& ec, std::size_t /*n*/) {
            if (ec) {
                log::debug(std::string("Write failed: ") + ec.message());
                stop();
                return;
            }
            (void)write_timer_.cancel();
            if (self->queued_bytes_ >= packet_size) {
                self->queued_bytes_ -= packet_size;
            } else {
                self->queued_bytes_ = 0;
            }
            self->send_queue_.pop(); // 제거되면서 pool 로 자동 반환된다.
            self->do_write();
        }));
}

void Session::send_hello() {
    // HELLO 메시지 구성
    // proto_major(2바이트): 주 프로토콜 버전 (호환성 체크용)
    // proto_minor(2바이트): 부 버전 (헤더 구조 v1.1 등)
    // capabilities(2바이트): 클라이언트 기능 지원 여부 (예: SENDER_SID)
    // heartbeat(2바이트): 서버가 요구하는 heartbeat 간격 (1/10초 단위)
    // epoch_high32(4바이트): 64비트 Timestamp 구성을 위한 상위 32비트 (Epoch)
    std::vector<std::uint8_t> payload_vec;
    payload_vec.resize(12);
    server::core::protocol::write_be16(server::core::protocol::kProtocolVersionMajor, payload_vec.data()); // proto_major
    server::core::protocol::write_be16(server::core::protocol::kProtocolVersionMinor, payload_vec.data() + 2); // proto_minor
    
    std::uint16_t caps = static_cast<std::uint16_t>(server::core::protocol::CAP_COMPRESS_SUPP | server::core::protocol::CAP_SENDER_SID);
    server::core::protocol::write_be16(caps, payload_vec.data() + 4);
    
    unsigned hb = options_ ? options_->heartbeat_interval_ms / 10 : 0;
    server::core::protocol::write_be16(static_cast<std::uint16_t>(hb), payload_vec.data() + 6);
    
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint32_t epoch_high32 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(now64) >> 32) & 0xFFFFFFFFu);
    server::core::protocol::write_be32(epoch_high32, payload_vec.data() + 8);
    std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);

    auto [buffer, packet_size] = make_packet(0x0001 /*MSG_HELLO*/, 0, payload_vec, tx_seq_++, now32);
    if (buffer) {
        async_send(std::move(buffer), packet_size);
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
    
    auto [buffer, packet_size] = make_packet(0x0004 /*MSG_ERR*/, 0, payload_vec, tx_seq_++, now32);
    if (buffer) {
        async_send(std::move(buffer), packet_size);
    }
}

void Session::arm_read_timeout() {
    if (!options_ || options_->read_timeout_ms == 0) return;
    auto self = shared_from_this();
    asio::dispatch(strand_, [this, self]() {
        // 읽기 타이머를 초기화한 뒤 다시 설정한다.
        (void)read_timer_.cancel();
        read_timer_.expires_after(std::chrono::milliseconds(options_->read_timeout_ms));
        read_timer_.async_wait(asio::bind_executor(strand_, [this, self](const error_code& ec) {
            if (ec || stopped_.load(std::memory_order_acquire)) return; // 타이머가 취소되었거나 세션이 종료된 경우 무시한다.
            runtime_metrics::record_session_timeout();
            if (options_ && options_->heartbeat_interval_ms > 0) {
                runtime_metrics::record_heartbeat_timeout();
            }
            log::warn("read timeout");
            stop();
        }));
    });
}

void Session::arm_write_timeout() {
    if (!options_ || options_->write_timeout_ms == 0) {
        return;
    }

    auto self = shared_from_this();
    (void)write_timer_.cancel();
    write_timer_.expires_after(std::chrono::milliseconds(options_->write_timeout_ms));
    write_timer_.async_wait(asio::bind_executor(strand_, [this, self](const error_code& ec) {
        if (ec || stopped_.load(std::memory_order_acquire)) {
            return;
        }
        runtime_metrics::record_session_write_timeout();
        log::warn("write timeout");
        stop();
    }));
}

void Session::arm_heartbeat() {
    if (!options_ || options_->heartbeat_interval_ms == 0) return;
    auto self = shared_from_this();
    asio::dispatch(strand_, [this, self]() {
        // heartbeat 주기를 유지하기 위해 타이머를 다시 설정한다.
        (void)heartbeat_timer_.cancel();
        heartbeat_timer_.expires_after(std::chrono::milliseconds(options_->heartbeat_interval_ms));
        heartbeat_timer_.async_wait(asio::bind_executor(strand_, [this, self](const error_code& ec) {
            if (ec || stopped_.load(std::memory_order_acquire)) return; // 타이머가 취소되었거나 세션이 이미 종료된 경우 무시한다.
            
            // PING 메시지 전송 (MSG_PING = 0x0002)
            // 페이로드는 비워둔다.
            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::uint32_t now32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
            
            auto [buffer, frame_size] = make_packet(0x0002 /*MSG_PING*/, 0, {}, tx_seq_++, now32);
            if (buffer) {
                async_send(std::move(buffer), frame_size);
            }

            // 다음 하트비트 예약
            arm_heartbeat();
        }));
    });
}

} // namespace server::core
