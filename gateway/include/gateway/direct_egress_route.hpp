#pragma once

#include <cstdint>

#include "server/protocol/game_opcodes.hpp"

namespace gateway {

enum class DirectEgressRoute : std::uint8_t {
    kTcpFallback = 0,
    kUdp,
    kRudp,
};

struct DirectEgressContext {
    std::uint16_t msg_id{0};
    bool udp_bound{false};
    bool rudp_selected{false};
    bool rudp_fallback_to_tcp{false};
    bool rudp_established{false};
};

inline bool is_direct_egress_msg(std::uint16_t msg_id) noexcept {
    return msg_id == server::protocol::MSG_FPS_STATE_DELTA;
}

inline DirectEgressRoute select_direct_egress_route(const DirectEgressContext& context) noexcept {
    if (!is_direct_egress_msg(context.msg_id) || !context.udp_bound) {
        return DirectEgressRoute::kTcpFallback;
    }

    if (!context.rudp_selected) {
        return DirectEgressRoute::kUdp;
    }

    if (context.rudp_fallback_to_tcp) {
        return DirectEgressRoute::kTcpFallback;
    }

    if (context.rudp_established) {
        return DirectEgressRoute::kRudp;
    }

    return DirectEgressRoute::kUdp;
}

} // namespace gateway
