#include <gtest/gtest.h>

#include "gateway/direct_egress_route.hpp"

TEST(DirectEgressRouteTest, UsesUdpForBoundNonRudpSessions) {
    const auto route = gateway::select_direct_egress_route(gateway::DirectEgressContext{
        .msg_id = server::protocol::MSG_FPS_STATE_DELTA,
        .udp_bound = true,
        .rudp_selected = false,
    });

    EXPECT_EQ(route, gateway::DirectEgressRoute::kUdp);
}

TEST(DirectEgressRouteTest, UsesRudpOnlyForEstablishedSelectedSessions) {
    const auto route = gateway::select_direct_egress_route(gateway::DirectEgressContext{
        .msg_id = server::protocol::MSG_FPS_STATE_DELTA,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = true,
    });

    EXPECT_EQ(route, gateway::DirectEgressRoute::kRudp);
}

TEST(DirectEgressRouteTest, FallsBackToTcpWhenRudpFallbackIsLatched) {
    const auto route = gateway::select_direct_egress_route(gateway::DirectEgressContext{
        .msg_id = server::protocol::MSG_FPS_STATE_DELTA,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_fallback_to_tcp = true,
        .rudp_established = true,
    });

    EXPECT_EQ(route, gateway::DirectEgressRoute::kTcpFallback);
}
