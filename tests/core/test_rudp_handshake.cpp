#include <gtest/gtest.h>

#include <server/core/net/rudp/rudp_engine.hpp>
#include <server/core/net/rudp/rudp_packet.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace {

std::vector<std::uint8_t> make_packet(server::core::net::rudp::PacketType type,
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

TEST(RudpHandshakeTest, HelloCreatesHelloAckAndDataInnerFrame) {
    server::core::net::rudp::RudpEngine engine;

    const auto hello = make_packet(server::core::net::rudp::PacketType::kHello, 77, 0);
    const auto hello_result = engine.process_datagram(hello, 1000);
    ASSERT_TRUE(hello_result.handshake_established);
    ASSERT_EQ(hello_result.egress_datagrams.size(), 1u);

    const auto decoded = server::core::net::rudp::decode_packet(hello_result.egress_datagrams.front());
    ASSERT_TRUE(decoded.ok);
    EXPECT_EQ(decoded.header.type, server::core::net::rudp::PacketType::kHelloAck);

    const std::vector<std::uint8_t> payload{0xAA, 0xBB, 0xCC, 0xDD};
    const auto data = make_packet(server::core::net::rudp::PacketType::kData, 77, 1, payload);
    const auto data_result = engine.process_datagram(data, 1020);
    ASSERT_EQ(data_result.inner_frames.size(), 1u);
    EXPECT_EQ(data_result.inner_frames.front(), payload);
}

TEST(RudpHandshakeTest, PollTriggersHandshakeAndIdleFallbacks) {
    server::core::net::rudp::RudpEngine engine;

    (void)engine.make_hello(99, 1000);
    const auto handshake_timeout = engine.poll(2600);
    EXPECT_TRUE(handshake_timeout.fallback_required);
    EXPECT_EQ(handshake_timeout.fallback_reason, "rudp_handshake_timeout");

    engine.reset();
    const auto hello = make_packet(server::core::net::rudp::PacketType::kHello, 99, 0);
    const auto established = engine.process_datagram(hello, 1000);
    ASSERT_TRUE(established.handshake_established);

    const auto idle_timeout = engine.poll(12050);
    EXPECT_TRUE(idle_timeout.fallback_required);
    EXPECT_EQ(idle_timeout.fallback_reason, "rudp_idle_timeout");
}

TEST(RudpHandshakeTest, DataBeforeHandshakeTriggersFallback) {
    server::core::net::rudp::RudpEngine engine;

    const std::vector<std::uint8_t> payload{0x01};
    const auto data = make_packet(server::core::net::rudp::PacketType::kData, 123, 1, payload);
    const auto result = engine.process_datagram(data, 1000);

    EXPECT_TRUE(result.parsed);
    EXPECT_TRUE(result.fallback_required);
    EXPECT_EQ(result.fallback_reason, "rudp_not_established");
}

TEST(RudpHandshakeTest, ConnectionIdMismatchTriggersFallback) {
    server::core::net::rudp::RudpEngine engine;

    const auto hello = make_packet(server::core::net::rudp::PacketType::kHello, 77, 0);
    const auto established = engine.process_datagram(hello, 1000);
    ASSERT_TRUE(established.handshake_established);

    const std::vector<std::uint8_t> payload{0x02};
    const auto mismatched = make_packet(server::core::net::rudp::PacketType::kData, 88, 1, payload);
    const auto result = engine.process_datagram(mismatched, 1010);

    EXPECT_TRUE(result.parsed);
    EXPECT_TRUE(result.fallback_required);
    EXPECT_EQ(result.fallback_reason, "connection_id_mismatch");
}

TEST(RudpHandshakeTest, TimeoutBoundaryRequiresStrictGreaterThan) {
    server::core::net::rudp::RudpEngine engine;

    (void)engine.make_hello(99, 1000);
    const auto handshake_boundary = engine.poll(2500);
    EXPECT_FALSE(handshake_boundary.fallback_required);

    const auto handshake_timeout = engine.poll(2501);
    EXPECT_TRUE(handshake_timeout.fallback_required);
    EXPECT_EQ(handshake_timeout.fallback_reason, "rudp_handshake_timeout");

    engine.reset();
    const auto hello = make_packet(server::core::net::rudp::PacketType::kHello, 99, 0);
    const auto established = engine.process_datagram(hello, 1000);
    ASSERT_TRUE(established.handshake_established);

    const auto idle_boundary = engine.poll(11000);
    EXPECT_FALSE(idle_boundary.fallback_required);

    const auto idle_timeout = engine.poll(11001);
    EXPECT_TRUE(idle_timeout.fallback_required);
    EXPECT_EQ(idle_timeout.fallback_reason, "rudp_idle_timeout");
}
