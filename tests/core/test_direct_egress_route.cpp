#include <gtest/gtest.h>

#include "server/core/realtime/direct_delivery.hpp"

TEST(DirectEgressRouteTest, UsesUdpForBoundNonRudpSessions) {
    const auto decision = server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = false,
    });

    EXPECT_EQ(decision.route, server::core::realtime::DirectDeliveryRoute::kUdp);
    EXPECT_EQ(decision.reason, server::core::realtime::DirectDeliveryReason::kUdpDirect);
}

TEST(DirectEgressRouteTest, UsesRudpOnlyForEstablishedSelectedSessions) {
    const auto decision = server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = true,
    });

    EXPECT_EQ(decision.route, server::core::realtime::DirectDeliveryRoute::kRudp);
    EXPECT_EQ(decision.reason, server::core::realtime::DirectDeliveryReason::kRudpDirect);
}

TEST(DirectEgressRouteTest, FallsBackToTcpWhenRudpFallbackIsLatched) {
    const auto decision = server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_fallback_to_tcp = true,
        .rudp_established = true,
    });

    EXPECT_EQ(decision.route, server::core::realtime::DirectDeliveryRoute::kTcpFallback);
    EXPECT_EQ(decision.reason, server::core::realtime::DirectDeliveryReason::kRudpFallbackLatched);
}

TEST(DirectEgressRouteTest, UsesUdpWhileRudpHandshakeIsPending) {
    const auto decision = server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = false,
    });

    EXPECT_EQ(decision.route, server::core::realtime::DirectDeliveryRoute::kUdp);
    EXPECT_EQ(decision.reason, server::core::realtime::DirectDeliveryReason::kRudpHandshakePendingUdp);
}
