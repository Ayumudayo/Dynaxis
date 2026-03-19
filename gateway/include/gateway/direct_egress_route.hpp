#pragma once

#include <cstdint>

#include "server/core/realtime/direct_delivery.hpp"
#include "server/protocol/game_opcodes.hpp"

namespace gateway {

using DirectEgressRoute = server::core::realtime::DirectDeliveryRoute;
using DirectEgressReason = server::core::realtime::DirectDeliveryReason;
using DirectEgressDecision = server::core::realtime::DirectDeliveryDecision;

/** @brief Direct UDP/RUDP egress eligibility inputs for a backend payload. */
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
    return server::core::realtime::select_direct_delivery_route(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = is_direct_egress_msg(context.msg_id),
        .udp_bound = context.udp_bound,
        .rudp_selected = context.rudp_selected,
        .rudp_fallback_to_tcp = context.rudp_fallback_to_tcp,
        .rudp_established = context.rudp_established,
    });
}

inline DirectEgressDecision evaluate_direct_egress(const DirectEgressContext& context) noexcept {
    return server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = is_direct_egress_msg(context.msg_id),
        .udp_bound = context.udp_bound,
        .rudp_selected = context.rudp_selected,
        .rudp_fallback_to_tcp = context.rudp_fallback_to_tcp,
        .rudp_established = context.rudp_established,
    });
}

} // namespace gateway
