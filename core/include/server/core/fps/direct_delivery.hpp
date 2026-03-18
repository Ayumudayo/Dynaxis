#pragma once

#include <cstdint>

namespace server::core::fps {

/** @brief Direct UDP/RUDP delivery route selected for a replication payload. */
enum class DirectDeliveryRoute : std::uint8_t {
    kTcpFallback = 0,
    kUdp,
    kRudp,
};

/** @brief Direct delivery decision reason, including fallback and recovery semantics. */
enum class DirectDeliveryReason : std::uint8_t {
    kMessageNotEligible = 0,
    kNotUdpBound,
    kUdpDirect,
    kRudpDirect,
    kRudpFallbackLatched,
    kRudpHandshakePendingUdp,
};

/** @brief Generic direct-delivery policy inputs independent of app-local opcode enums. */
struct DirectDeliveryContext {
    bool direct_path_enabled_for_message{false};
    bool udp_bound{false};
    bool rudp_selected{false};
    bool rudp_fallback_to_tcp{false};
    bool rudp_established{false};
};

/** @brief Explicit direct delivery decision result. */
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

} // namespace server::core::fps
