#pragma once

#include <cstdint>

namespace server::core::realtime {

/**
 * @brief Tracks sequenced direct-UDP transport quality for one bound session.
 */
class UdpSequencedMetrics {
public:
    /** @brief Per-packet classification result and quality deltas. */
    struct UpdateResult {
        bool accepted{false};                    ///< Accepted as forward progress.
        bool duplicate{false};                   ///< Same sequence as the last accepted packet.
        bool reordered{false};                   ///< Older than the last accepted packet.
        std::uint64_t estimated_lost_packets{0}; ///< Missing packets inferred before this packet.
        std::uint64_t jitter_ms{0};              ///< Inter-arrival jitter delta in milliseconds.
    };

    /** @brief Resets the tracker, typically after a fresh bind/rebind. */
    void reset() {
        initialized_ = false;
        last_seq_ = 0;
        last_recv_ms_ = 0;
        last_interarrival_ms_ = 0;
    }

    /**
     * @brief Consumes one packet sample and updates quality statistics.
     * @param seq Packet sequence number from the transport header.
     * @param recv_unix_ms Packet receive timestamp in unix milliseconds.
     * @return Packet classification result and derived quality deltas.
     */
    [[nodiscard]] UpdateResult on_packet(std::uint32_t seq, std::uint64_t recv_unix_ms) {
        UpdateResult result{};

        if (!initialized_) {
            initialized_ = true;
            last_seq_ = seq;
            last_recv_ms_ = recv_unix_ms;
            last_interarrival_ms_ = 0;
            result.accepted = true;
            return result;
        }

        if (seq == last_seq_) {
            result.duplicate = true;
            return result;
        }

        if (seq < last_seq_) {
            result.reordered = true;
            return result;
        }

        result.accepted = true;
        if (seq > (last_seq_ + 1u)) {
            result.estimated_lost_packets = static_cast<std::uint64_t>(seq - last_seq_ - 1u);
        }

        if (last_recv_ms_ != 0) {
            if (recv_unix_ms >= last_recv_ms_) {
                const auto interarrival_ms = recv_unix_ms - last_recv_ms_;
                if (last_interarrival_ms_ != 0) {
                    result.jitter_ms = (interarrival_ms >= last_interarrival_ms_)
                        ? (interarrival_ms - last_interarrival_ms_)
                        : (last_interarrival_ms_ - interarrival_ms);
                }
                last_interarrival_ms_ = interarrival_ms;
            } else {
                last_interarrival_ms_ = 0;
            }
        }

        last_seq_ = seq;
        last_recv_ms_ = recv_unix_ms;
        return result;
    }

private:
    bool initialized_{false};
    std::uint32_t last_seq_{0};
    std::uint64_t last_recv_ms_{0};
    std::uint64_t last_interarrival_ms_{0};
};

} // namespace server::core::realtime
