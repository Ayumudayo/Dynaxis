#include "server/core/net/rudp/rudp_engine.hpp"

#include <algorithm>
#include <utility>

namespace server::core::net::rudp {

namespace {

std::uint32_t now32(std::uint64_t now_unix_ms) {
    return static_cast<std::uint32_t>(now_unix_ms & 0xFFFFFFFFu);
}

} // namespace

RudpEngine::RudpEngine(RudpConfig config)
    : config_(std::move(config)) {}

void RudpEngine::reset() {
    state_.reset();
    ack_window_.reset();
    retransmission_queue_.reset();
}

std::vector<std::uint8_t> RudpEngine::make_packet(PacketType type,
                                                  std::uint8_t flags,
                                                  std::uint8_t channel,
                                                  std::uint32_t packet_number,
                                                  std::uint64_t now_unix_ms,
                                                  std::span<const std::uint8_t> payload) const {
    RudpHeader header{};
    header.type = type;
    header.flags = flags;
    header.channel = channel;
    header.connection_id = state_.connection_id;
    header.packet_number = packet_number;
    header.ack_largest = state_.ack_largest;
    header.ack_mask = state_.ack_mask;
    header.ack_delay_ms = static_cast<std::uint16_t>(std::min<std::uint32_t>(config_.ack_delay_ms, 0xFFFFu));
    header.timestamp_ms = now32(now_unix_ms);
    header.payload_length = static_cast<std::uint16_t>(payload.size());
    return encode_packet(header, payload);
}

void RudpEngine::set_fallback(ProcessResult& result, std::string reason) {
    state_.mark_fallback(reason);
    state_.lifecycle = LifecycleState::kDraining;
    result.fallback_required = true;
    result.fallback_reason = std::move(state_.fallback_reason);
}

std::vector<std::uint8_t> RudpEngine::make_hello(std::uint32_t connection_id, std::uint64_t now_unix_ms) {
    state_.connection_id = connection_id;
    state_.lifecycle = LifecycleState::kHelloSent;
    state_.last_send_unix_ms = now_unix_ms;
    return make_packet(PacketType::kHello, 0, 0, 0, now_unix_ms, {});
}

std::vector<std::uint8_t> RudpEngine::make_close(std::uint64_t now_unix_ms) {
    state_.lifecycle = LifecycleState::kClosed;
    state_.last_send_unix_ms = now_unix_ms;
    return make_packet(PacketType::kClose, 0, 0, 0, now_unix_ms, {});
}

ProcessResult RudpEngine::process_datagram(std::span<const std::uint8_t> datagram, std::uint64_t now_unix_ms) {
    ProcessResult result{};
    if (!looks_like_rudp(datagram)) {
        return result;
    }

    result.consumed = true;
    auto decoded = decode_packet(datagram);
    if (!decoded.ok || decoded.header.version != 1) {
        set_fallback(result, "invalid_rudp_packet");
        return result;
    }

    result.parsed = true;
    state_.last_recv_unix_ms = now_unix_ms;

    if (state_.connection_id != 0
        && decoded.header.connection_id != 0
        && state_.connection_id != decoded.header.connection_id) {
        set_fallback(result, "connection_id_mismatch");
        return result;
    }

    if (state_.connection_id == 0) {
        state_.connection_id = decoded.header.connection_id;
    }

    retransmission_queue_.mark_acked(decoded.header.ack_largest, decoded.header.ack_mask);

    switch (decoded.header.type) {
    case PacketType::kHello: {
        const bool new_established = (state_.lifecycle != LifecycleState::kEstablished);
        state_.lifecycle = LifecycleState::kEstablished;
        state_.last_send_unix_ms = now_unix_ms;
        result.handshake_established = new_established;
        result.egress_datagrams.push_back(make_packet(PacketType::kHelloAck, 0, 0, 0, now_unix_ms, {}));
        return result;
    }
    case PacketType::kHelloAck:
        state_.lifecycle = LifecycleState::kEstablished;
        result.handshake_established = true;
        return result;
    case PacketType::kClose:
        state_.lifecycle = LifecycleState::kClosed;
        set_fallback(result, "peer_closed");
        return result;
    case PacketType::kPing:
        result.egress_datagrams.push_back(make_packet(PacketType::kData, kFlagAckOnly, 0, 0, now_unix_ms, {}));
        return result;
    case PacketType::kData:
        break;
    }

    if (state_.lifecycle != LifecycleState::kEstablished) {
        set_fallback(result, "rudp_not_established");
        return result;
    }

    const auto ack = ack_window_.observe(decoded.header.packet_number);
    state_.ack_largest = ack.ack_largest;
    state_.ack_mask = ack.ack_mask;

    if (!ack.accepted) {
        return result;
    }

    if (!decoded.payload.empty()) {
        result.inner_frames.emplace_back(decoded.payload.begin(), decoded.payload.end());
    }

    const bool should_emit_ack =
        (decoded.header.flags & kFlagAckOnly) == 0
        && (now_unix_ms >= (state_.last_ack_emit_unix_ms + config_.ack_delay_ms));
    if (should_emit_ack) {
        result.egress_datagrams.push_back(make_packet(PacketType::kData, kFlagAckOnly, decoded.header.channel, 0, now_unix_ms, {}));
        state_.last_ack_emit_unix_ms = now_unix_ms;
    }

    if (decoded.header.timestamp_ms != 0) {
        const auto sent_ts = static_cast<std::uint64_t>(decoded.header.timestamp_ms);
        const auto now_ts = now_unix_ms & 0xFFFFFFFFu;
        const auto sample = static_cast<std::uint32_t>((now_ts >= sent_ts) ? (now_ts - sent_ts) : 0);
        state_.rtt.update(sample, config_.rto_min_ms, config_.rto_max_ms);
    }

    return result;
}

bool RudpEngine::queue_reliable_payload(std::span<const std::uint8_t> inner_frame,
                                        std::uint8_t channel,
                                        std::uint64_t now_unix_ms,
                                        std::vector<std::uint8_t>& out_datagram) {
    if (state_.lifecycle != LifecycleState::kEstablished) {
        return false;
    }

    if (inner_frame.empty()) {
        return false;
    }

    if (inner_frame.size() > config_.mtu_payload_bytes) {
        return false;
    }

    if (retransmission_queue_.inflight_packets() >= config_.max_inflight_packets) {
        return false;
    }

    if (retransmission_queue_.inflight_bytes() + inner_frame.size() > config_.max_inflight_bytes) {
        return false;
    }

    const auto packet_number = state_.next_packet_number++;
    auto frame = make_packet(PacketType::kData, 0, channel, packet_number, now_unix_ms, inner_frame);
    retransmission_queue_.push(packet_number, now_unix_ms, frame);
    state_.last_send_unix_ms = now_unix_ms;
    out_datagram = std::move(frame);
    return true;
}

ProcessResult RudpEngine::poll(std::uint64_t now_unix_ms) {
    ProcessResult result{};

    if (state_.lifecycle == LifecycleState::kHelloSent
        && state_.last_send_unix_ms > 0
        && now_unix_ms > (state_.last_send_unix_ms + config_.handshake_timeout_ms)) {
        set_fallback(result, "rudp_handshake_timeout");
        return result;
    }

    if (state_.lifecycle == LifecycleState::kEstablished
        && state_.last_recv_unix_ms > 0
        && now_unix_ms > (state_.last_recv_unix_ms + config_.idle_timeout_ms)) {
        set_fallback(result, "rudp_idle_timeout");
        return result;
    }

    const auto due = retransmission_queue_.collect_timeouts(now_unix_ms, state_.rtt.rto_ms, 16);
    if (due.empty()) {
        return result;
    }

    result.retransmit_count = due.size();
    result.egress_datagrams.reserve(due.size());
    for (const auto& packet : due) {
        auto frame = packet.encoded_frame;
        if (frame.size() >= kRudpHeaderBytes) {
            frame[27] = static_cast<std::uint8_t>(frame[27] | kFlagRetransmit);
        }
        result.egress_datagrams.push_back(std::move(frame));
    }
    return result;
}

} // namespace server::core::net::rudp
