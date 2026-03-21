#pragma once

#include <cstdint>

namespace server::core::realtime {

/** @brief replication payload에 대해 선택된 direct UDP/RUDP 전송 경로입니다. */
enum class DirectDeliveryRoute : std::uint8_t {
    kTcpFallback = 0,
    kUdp,
    kRudp,
};

/** @brief direct delivery 판단 이유입니다. fallback과 handshake 대기 같은 회복 의미도 함께 담습니다. */
enum class DirectDeliveryReason : std::uint8_t {
    kMessageNotEligible = 0,
    kNotUdpBound,
    kUdpDirect,
    kRudpDirect,
    kRudpFallbackLatched,
    kRudpHandshakePendingUdp,
};

/**
 * @brief app-local opcode enum과 독립적인 direct-delivery 정책 입력입니다.
 *
 * direct path 판단을 별도 구조체로 두는 이유는, gateway/server가 같은 판단 규칙을 공유하되
 * 각자 가진 메시지 열거형이나 wire 해석 세부에 직접 결합하지 않게 하기 위해서입니다.
 */
struct DirectDeliveryContext {
    bool direct_path_enabled_for_message{false};
    bool udp_bound{false};
    bool rudp_selected{false};
    bool rudp_fallback_to_tcp{false};
    bool rudp_established{false};
};

/** @brief 명시적인 direct delivery 판단 결과입니다. */
struct DirectDeliveryDecision {
    DirectDeliveryRoute route{DirectDeliveryRoute::kTcpFallback};
    DirectDeliveryReason reason{DirectDeliveryReason::kMessageNotEligible};
};

inline DirectDeliveryDecision evaluate_direct_delivery(const DirectDeliveryContext& context) noexcept {
    if (!context.direct_path_enabled_for_message || !context.udp_bound) {
        return DirectDeliveryDecision{
            .route = DirectDeliveryRoute::kTcpFallback,
            .reason = context.direct_path_enabled_for_message
                ? DirectDeliveryReason::kNotUdpBound
                : DirectDeliveryReason::kMessageNotEligible,
        };
    }

    if (!context.rudp_selected) {
        return DirectDeliveryDecision{
            .route = DirectDeliveryRoute::kUdp,
            .reason = DirectDeliveryReason::kUdpDirect,
        };
    }

    if (context.rudp_fallback_to_tcp) {
        return DirectDeliveryDecision{
            .route = DirectDeliveryRoute::kTcpFallback,
            .reason = DirectDeliveryReason::kRudpFallbackLatched,
        };
    }

    if (context.rudp_established) {
        return DirectDeliveryDecision{
            .route = DirectDeliveryRoute::kRudp,
            .reason = DirectDeliveryReason::kRudpDirect,
        };
    }

    return DirectDeliveryDecision{
        .route = DirectDeliveryRoute::kUdp,
        .reason = DirectDeliveryReason::kRudpHandshakePendingUdp,
    };
}

inline DirectDeliveryRoute select_direct_delivery_route(const DirectDeliveryContext& context) noexcept {
    return evaluate_direct_delivery(context).route;
}

} // namespace server::core::realtime
