#include "server/core/net/rudp/retransmission_queue.hpp"

#include <algorithm>

namespace server::core::net::rudp {

void RetransmissionQueue::reset() {
    pending_.clear();
    inflight_bytes_ = 0;
}

void RetransmissionQueue::push(std::uint32_t packet_number,
                               std::uint64_t sent_unix_ms,
                               std::vector<std::uint8_t> encoded_frame) {
    if (encoded_frame.empty()) {
        return;
    }

    Entry entry{};
    entry.packet.packet_number = packet_number;
    entry.packet.first_sent_unix_ms = sent_unix_ms;
    entry.packet.last_sent_unix_ms = sent_unix_ms;
    entry.packet.retransmit_count = 0;
    entry.packet.encoded_frame = std::move(encoded_frame);
    inflight_bytes_ += entry.packet.encoded_frame.size();
    pending_.push_back(std::move(entry));
}

bool RetransmissionQueue::is_acked_by_mask(std::uint32_t packet_number,
                                           std::uint32_t ack_largest,
                                           std::uint64_t ack_mask) {
    if (packet_number > ack_largest) {
        return false;
    }
    if (packet_number == ack_largest) {
        return true;
    }

    const auto distance = ack_largest - packet_number;
    if (distance >= 64) {
        return true;
    }

    const std::uint64_t bit = 1ULL << distance;
    return (ack_mask & bit) != 0;
}

void RetransmissionQueue::erase_acked() {
    while (!pending_.empty() && pending_.front().acked) {
        const auto size = pending_.front().packet.encoded_frame.size();
        if (inflight_bytes_ >= size) {
            inflight_bytes_ -= size;
        } else {
            inflight_bytes_ = 0;
        }
        pending_.pop_front();
    }

    if (pending_.empty()) {
        return;
    }

    const auto original_size = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Entry& entry) {
        return entry.acked;
    }), pending_.end());

    if (pending_.size() == original_size) {
        return;
    }

    inflight_bytes_ = 0;
    for (const auto& entry : pending_) {
        inflight_bytes_ += entry.packet.encoded_frame.size();
    }
}

void RetransmissionQueue::mark_acked(std::uint32_t ack_largest, std::uint64_t ack_mask) {
    for (auto& entry : pending_) {
        if (!entry.acked
            && is_acked_by_mask(entry.packet.packet_number, ack_largest, ack_mask)) {
            entry.acked = true;
        }
    }
    erase_acked();
}

std::vector<PendingPacket> RetransmissionQueue::collect_timeouts(std::uint64_t now_unix_ms,
                                                                 std::uint32_t rto_ms,
                                                                 std::size_t max_batch) {
    std::vector<PendingPacket> due;
    if (max_batch == 0 || rto_ms == 0) {
        return due;
    }

    due.reserve(std::min(max_batch, pending_.size()));
    for (auto& entry : pending_) {
        if (entry.acked) {
            continue;
        }

        const auto elapsed = now_unix_ms - entry.packet.last_sent_unix_ms;
        if (elapsed < static_cast<std::uint64_t>(rto_ms)) {
            continue;
        }

        entry.packet.last_sent_unix_ms = now_unix_ms;
        entry.packet.retransmit_count += 1;
        due.push_back(entry.packet);
        if (due.size() >= max_batch) {
            break;
        }
    }
    return due;
}

} // namespace server::core::net::rudp
