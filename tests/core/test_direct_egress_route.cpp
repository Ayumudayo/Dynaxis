#include <gtest/gtest.h>

#include "server/core/fps/direct_delivery.hpp"

TEST(DirectEgressRouteTest, UsesUdpForBoundNonRudpSessions) {
    const auto decision = server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = false,
    });

    EXPECT_EQ(decision.route, server::core::fps::DirectDeliveryRoute::kUdp);
    EXPECT_EQ(decision.reason, server::core::fps::DirectDeliveryReason::kUdpDirect);
}

TEST(DirectEgressRouteTest, UsesRudpOnlyForEstablishedSelectedSessions) {
    const auto decision = server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = true,
    });

    EXPECT_EQ(decision.route, server::core::fps::DirectDeliveryRoute::kRudp);
    EXPECT_EQ(decision.reason, server::core::fps::DirectDeliveryReason::kRudpDirect);
}

TEST(DirectEgressRouteTest, FallsBackToTcpWhenRudpFallbackIsLatched) {
    const auto decision = server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_fallback_to_tcp = true,
        .rudp_established = true,
    });

    EXPECT_EQ(decision.route, server::core::fps::DirectDeliveryRoute::kTcpFallback);
    EXPECT_EQ(decision.reason, server::core::fps::DirectDeliveryReason::kRudpFallbackLatched);
}

TEST(DirectEgressRouteTest, UsesUdpWhileRudpHandshakeIsPending) {
    const auto decision = server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = false,
    });

    EXPECT_EQ(decision.route, server::core::fps::DirectDeliveryRoute::kUdp);
    EXPECT_EQ(decision.reason, server::core::fps::DirectDeliveryReason::kRudpHandshakePendingUdp);
}
