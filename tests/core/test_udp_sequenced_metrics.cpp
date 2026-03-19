#include <gtest/gtest.h>

#include <server/core/realtime/transport_quality.hpp>

TEST(UdpSequencedMetricsTest, AcceptsFirstPacket) {
    server::core::realtime::UdpSequencedMetrics metrics;

    const auto result = metrics.on_packet(10, 1000);
    EXPECT_TRUE(result.accepted);
    EXPECT_FALSE(result.duplicate);
    EXPECT_FALSE(result.reordered);
    EXPECT_EQ(result.estimated_lost_packets, 0);
    EXPECT_EQ(result.jitter_ms, 0);
}

TEST(UdpSequencedMetricsTest, MarksDuplicatePacket) {
    server::core::realtime::UdpSequencedMetrics metrics;

    EXPECT_TRUE(metrics.on_packet(11, 1000).accepted);
    const auto duplicate = metrics.on_packet(11, 1010);
    EXPECT_FALSE(duplicate.accepted);
    EXPECT_TRUE(duplicate.duplicate);
    EXPECT_FALSE(duplicate.reordered);
}

TEST(UdpSequencedMetricsTest, MarksReorderedPacket) {
    server::core::realtime::UdpSequencedMetrics metrics;

    EXPECT_TRUE(metrics.on_packet(20, 1000).accepted);
    EXPECT_TRUE(metrics.on_packet(22, 1010).accepted);

    const auto reordered = metrics.on_packet(21, 1020);
    EXPECT_FALSE(reordered.accepted);
    EXPECT_FALSE(reordered.duplicate);
    EXPECT_TRUE(reordered.reordered);
}

TEST(UdpSequencedMetricsTest, EstimatesLossAndJitter) {
    server::core::realtime::UdpSequencedMetrics metrics;

    EXPECT_TRUE(metrics.on_packet(100, 1000).accepted);

    const auto second = metrics.on_packet(102, 1015);
    EXPECT_TRUE(second.accepted);
    EXPECT_EQ(second.estimated_lost_packets, 1);
    EXPECT_EQ(second.jitter_ms, 0);

    const auto third = metrics.on_packet(103, 1040);
    EXPECT_TRUE(third.accepted);
    EXPECT_EQ(third.estimated_lost_packets, 0);
    EXPECT_EQ(third.jitter_ms, 10);
}

TEST(UdpSequencedMetricsTest, ResetClearsReplayWindowForRebind) {
    server::core::realtime::UdpSequencedMetrics metrics;

    EXPECT_TRUE(metrics.on_packet(500, 1000).accepted);
    EXPECT_FALSE(metrics.on_packet(499, 1010).accepted);

    metrics.reset();

    const auto after_rebind = metrics.on_packet(499, 1020);
    EXPECT_TRUE(after_rebind.accepted);
    EXPECT_FALSE(after_rebind.duplicate);
    EXPECT_FALSE(after_rebind.reordered);
}
