#include <gtest/gtest.h>

#include <server/core/net/rudp/rudp_engine.hpp>
#include <server/core/net/rudp/rudp_packet.hpp>

#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> make_hello(std::uint32_t connection_id) {
    server::core::net::rudp::RudpHeader header{};
    header.type = server::core::net::rudp::PacketType::kHello;
    header.connection_id = connection_id;
    return server::core::net::rudp::encode_packet(header, {});
}

void establish(server::core::net::rudp::RudpEngine& engine, std::uint32_t connection_id) {
    const auto hello = make_hello(connection_id);
    const auto result = engine.process_datagram(hello, 1000);
    ASSERT_TRUE(result.handshake_established);
}

} // namespace

TEST(RudpFlowControlTest, RejectsPayloadAboveConfiguredMtu) {
    server::core::net::rudp::RudpConfig config{};
    config.mtu_payload_bytes = 4;
    server::core::net::rudp::RudpEngine engine(config);
    establish(engine, 1234);

    std::vector<std::uint8_t> datagram;
    const std::vector<std::uint8_t> too_big{1, 2, 3, 4, 5};
    EXPECT_FALSE(engine.queue_reliable_payload(too_big, 0, 1010, datagram));
}

TEST(RudpFlowControlTest, RejectsWhenInflightPacketLimitExceeded) {
    server::core::net::rudp::RudpConfig config{};
    config.max_inflight_packets = 1;
    config.max_inflight_bytes = 1024;
    server::core::net::rudp::RudpEngine engine(config);
    establish(engine, 4321);

    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    const std::vector<std::uint8_t> payload{0x01, 0x02};

    ASSERT_TRUE(engine.queue_reliable_payload(payload, 0, 1010, first));
    EXPECT_FALSE(engine.queue_reliable_payload(payload, 0, 1011, second));
}

TEST(RudpFlowControlTest, RejectsWhenInflightByteLimitExceeded) {
    server::core::net::rudp::RudpConfig config{};
    config.max_inflight_packets = 8;
    config.max_inflight_bytes = 4;
    server::core::net::rudp::RudpEngine engine(config);
    establish(engine, 777);

    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    const std::vector<std::uint8_t> four_bytes{1, 2, 3, 4};
    const std::vector<std::uint8_t> one_byte{9};

    ASSERT_TRUE(engine.queue_reliable_payload(four_bytes, 0, 1010, first));
    EXPECT_FALSE(engine.queue_reliable_payload(one_byte, 0, 1011, second));
}

TEST(RudpFlowControlTest, QueueRequiresEstablishedStateAndNonEmptyPayload) {
    server::core::net::rudp::RudpEngine engine;

    std::vector<std::uint8_t> datagram;
    const std::vector<std::uint8_t> payload{0x01};
    EXPECT_FALSE(engine.queue_reliable_payload(payload, 0, 1000, datagram));

    establish(engine, 2468);
    const std::vector<std::uint8_t> empty;
    EXPECT_FALSE(engine.queue_reliable_payload(empty, 0, 1010, datagram));
}
