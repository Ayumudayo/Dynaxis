#include <gtest/gtest.h>

#include <server/core/fps/direct_bind.hpp>

TEST(DirectBindContractTest, RequestPayloadRoundTrips) {
    const auto payload = server::core::fps::encode_direct_bind_request_payload(server::core::fps::DirectBindRequest{
        .session_id = "session-a",
        .nonce = 11,
        .expires_unix_ms = 22,
        .token = "token",
    });

    server::core::fps::DirectBindRequest decoded{};
    ASSERT_TRUE(server::core::fps::decode_direct_bind_request_payload(payload, decoded));
    EXPECT_EQ(decoded.session_id, "session-a");
    EXPECT_EQ(decoded.nonce, 11u);
    EXPECT_EQ(decoded.expires_unix_ms, 22u);
    EXPECT_EQ(decoded.token, "token");
}

TEST(DirectBindContractTest, ResponsePayloadRoundTrips) {
    const auto payload = server::core::fps::encode_direct_bind_response_payload(
        7,
        server::core::fps::DirectBindTicket{
            .session_id = "session-a",
            .nonce = 11,
            .expires_unix_ms = 22,
            .token = "token",
        },
        "issued");

    server::core::fps::DirectBindResponse decoded{};
    ASSERT_TRUE(server::core::fps::decode_direct_bind_response_payload(payload, decoded));
    EXPECT_EQ(decoded.code, 7u);
    EXPECT_EQ(decoded.ticket.session_id, "session-a");
    EXPECT_EQ(decoded.ticket.nonce, 11u);
    EXPECT_EQ(decoded.ticket.expires_unix_ms, 22u);
    EXPECT_EQ(decoded.ticket.token, "token");
    EXPECT_EQ(decoded.message, "issued");
}

TEST(DirectBindContractTest, RejectsTruncatedPayload) {
    const std::vector<std::uint8_t> truncated{0x00, 0x04, 'a'};
    server::core::fps::DirectBindRequest decoded{};
    EXPECT_FALSE(server::core::fps::decode_direct_bind_request_payload(truncated, decoded));
}
