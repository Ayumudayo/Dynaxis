#include "server/core/net/rudp/ack_window.hpp"

namespace server::core::net::rudp {

void AckWindow::reset() {
    initialized_ = false;
    largest_ = 0;
    mask_ = 0;
}

AckObservation AckWindow::observe(std::uint32_t packet_number) {
    AckObservation observation{};

    if (!initialized_) {
        initialized_ = true;
        largest_ = packet_number;
        mask_ = 1;
        observation.accepted = true;
        observation.ack_largest = largest_;
        observation.ack_mask = mask_;
        return observation;
    }

    if (packet_number > largest_) {
        const auto delta = packet_number - largest_;
        if (delta > 1) {
            observation.estimated_lost_packets = static_cast<std::uint64_t>(delta - 1);
        }

        if (delta >= 64) {
            mask_ = 1;
        } else {
            mask_ = (mask_ << delta) | 1ULL;
        }

        largest_ = packet_number;
        observation.accepted = true;
        observation.ack_largest = largest_;
        observation.ack_mask = mask_;
        return observation;
    }

    const auto distance = largest_ - packet_number;
    if (distance >= 64) {
        observation.duplicate = true;
        observation.accepted = false;
        observation.ack_largest = largest_;
        observation.ack_mask = mask_;
        return observation;
    }

    const std::uint64_t bit = (1ULL << distance);
    if ((mask_ & bit) != 0) {
        observation.duplicate = true;
        observation.accepted = false;
        observation.ack_largest = largest_;
        observation.ack_mask = mask_;
        return observation;
    }

    mask_ |= bit;
    observation.accepted = true;
    observation.reordered = true;
    observation.ack_largest = largest_;
    observation.ack_mask = mask_;
    return observation;
}

} // namespace server::core::net::rudp
