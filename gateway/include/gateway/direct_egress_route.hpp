#pragma once

#include <cstdint>

#include "server/core/realtime/direct_delivery.hpp"
#include "server/protocol/game_opcodes.hpp"

namespace gateway {

using DirectEgressRoute = server::core::realtime::DirectDeliveryRoute;
using DirectEgressReason = server::core::realtime::DirectDeliveryReason;
using DirectEgressDecision = server::core::realtime::DirectDeliveryDecision;

/**
 * @brief backend payload가 direct UDP/RUDP egress 대상인지 판단할 때 쓰는 입력값입니다.
 *
 * gateway는 모든 outbound payload를 direct 경로로 보내지 않습니다. 이 문맥 구조체는
 * 메시지 종류와 현재 세션의 UDP/RUDP 상태를 한곳에 모아, "지금 direct path를 써도
 * 되는가"를 일관된 규칙으로 평가하게 합니다.
 */
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
