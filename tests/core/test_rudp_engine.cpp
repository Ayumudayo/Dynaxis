#include <gtest/gtest.h>

#include <server/core/net/rudp/ack_window.hpp>
#include <server/core/net/rudp/retransmission_queue.hpp>
#include <server/core/net/rudp/rudp_engine.hpp>
#include <server/core/net/rudp/rudp_packet.hpp>
#include <server/core/runtime_metrics.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace {

std::vector<std::uint8_t> make_rudp_packet(server::core::net::rudp::PacketType type,
                                           std::uint32_t connection_id,
                                           std::uint32_t packet_number,
                                           std::span<const std::uint8_t> payload = {}) {
    server::core::net::rudp::RudpHeader header{};
    header.type = type;
    header.connection_id = connection_id;
    header.packet_number = packet_number;
    return server::core::net::rudp::encode_packet(header, payload);
}

} // namespace

TEST(RudpAckWindowTest, TracksLargestMaskAndDuplicates) {
    server::core::net::rudp::AckWindow window;

    const auto first = window.observe(10);
    EXPECT_TRUE(first.accepted);
    EXPECT_EQ(first.ack_largest, 10u);
    EXPECT_EQ(first.ack_mask, 1u);

    const auto gap = window.observe(12);
    EXPECT_TRUE(gap.accepted);
    EXPECT_EQ(gap.estimated_lost_packets, 1u);
    EXPECT_EQ(gap.ack_largest, 12u);

    const auto reorder = window.observe(11);
    EXPECT_TRUE(reorder.accepted);
    EXPECT_TRUE(reorder.reordered);

    const auto duplicate = window.observe(11);
    EXPECT_FALSE(duplicate.accepted);
    EXPECT_TRUE(duplicate.duplicate);
}

TEST(RudpRetransmissionQueueTest, MarksAckedAndCollectsTimeouts) {
    server::core::net::rudp::RetransmissionQueue queue;
    queue.push(1, 1000, {0x01, 0x02});
    queue.push(2, 1000, {0x03, 0x04});
    EXPECT_EQ(queue.inflight_packets(), 2u);

    queue.mark_acked(1, 1);
    EXPECT_EQ(queue.inflight_packets(), 1u);

    const auto due = queue.collect_timeouts(1300, 200, 8);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due.front().packet_number, 2u);
    EXPECT_EQ(due.front().retransmit_count, 1u);
}

TEST(RudpEngineTest, HandshakeAndDataFlow) {
    server::core::net::rudp::RudpEngine engine;

    const auto hello = make_rudp_packet(server::core::net::rudp::PacketType::kHello, 42, 0);
    const auto hello_result = engine.process_datagram(hello, 1000);
    EXPECT_TRUE(hello_result.parsed);
    EXPECT_TRUE(hello_result.handshake_established);
    ASSERT_EQ(hello_result.egress_datagrams.size(), 1u);

    const std::vector<std::uint8_t> payload = {0xAA, 0xBB, 0xCC};
    const auto data = make_rudp_packet(server::core::net::rudp::PacketType::kData, 42, 1, payload);
    const auto data_result = engine.process_datagram(data, 1020);
    ASSERT_EQ(data_result.inner_frames.size(), 1u);
    EXPECT_EQ(data_result.inner_frames.front(), payload);
}

TEST(RudpEngineTest, PollRetransmitsQueuedPayload) {
    server::core::net::rudp::RudpEngine engine;

    const auto hello = make_rudp_packet(server::core::net::rudp::PacketType::kHello, 7, 0);
    const auto hello_result = engine.process_datagram(hello, 1000);
    ASSERT_TRUE(hello_result.handshake_established);

    std::vector<std::uint8_t> outbound;
    const std::vector<std::uint8_t> payload = {0x01, 0x02, 0x03};
    ASSERT_TRUE(engine.queue_reliable_payload(payload, 0, 1010, outbound));
    EXPECT_FALSE(outbound.empty());

    const auto retransmit = engine.poll(1300);
    ASSERT_EQ(retransmit.retransmit_count, 1u);
    ASSERT_EQ(retransmit.egress_datagrams.size(), 1u);
}

TEST(RudpEngineTest, UnreliableQueueDoesNotCreateRetransmitDebt) {
    server::core::net::rudp::RudpEngine engine;

    const auto hello = make_rudp_packet(server::core::net::rudp::PacketType::kHello, 8, 0);
    const auto hello_result = engine.process_datagram(hello, 1000);
    ASSERT_TRUE(hello_result.handshake_established);

    std::vector<std::uint8_t> outbound;
    const std::vector<std::uint8_t> payload = {0x0A, 0x0B, 0x0C};
    ASSERT_TRUE(engine.queue_unreliable_payload(payload, 1, 1010, outbound));
    EXPECT_FALSE(outbound.empty());

    const auto retransmit = engine.poll(1300);
    EXPECT_EQ(retransmit.retransmit_count, 0u);
    EXPECT_TRUE(retransmit.egress_datagrams.empty());
}

TEST(RudpEngineTest, RuntimeMetricsSignalsIncrease) {
    const auto before = server::core::runtime_metrics::snapshot();

    server::core::net::rudp::RudpEngine engine;

    const auto hello = make_rudp_packet(server::core::net::rudp::PacketType::kHello, 100, 0);
    (void)engine.process_datagram(hello, 2000);

    std::vector<std::uint8_t> outbound;
    const std::vector<std::uint8_t> payload = {0x10, 0x20};
    (void)engine.queue_reliable_payload(payload, 0, 2020, outbound);
    (void)engine.poll(2300);

    // invalid version packet to force protocol fallback
    auto invalid = hello;
    invalid[2] = 9;
    (void)engine.process_datagram(invalid, 2010);

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_GE(after.rudp_handshake_ok_total, before.rudp_handshake_ok_total + 1);
    EXPECT_GE(after.rudp_handshake_fail_total, before.rudp_handshake_fail_total + 1);
    EXPECT_GE(after.rudp_fallback_total[static_cast<std::size_t>(server::core::runtime_metrics::RudpFallbackReason::kProtocolError)],
              before.rudp_fallback_total[static_cast<std::size_t>(server::core::runtime_metrics::RudpFallbackReason::kProtocolError)] + 1);
}
