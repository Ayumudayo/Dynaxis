#include <gtest/gtest.h>

#include <server/core/realtime/transport_policy.hpp>

#include <cstdint>

TEST(RudpRolloutPolicyTest, ParseAllowlistSupportsDecimalAndHexTokens) {
    const auto allowlist = server::core::realtime::parse_direct_opcode_allowlist("100, 0x2A,0X0001,invalid,,65536");

    EXPECT_EQ(allowlist.size(), 3u);
    EXPECT_TRUE(allowlist.contains(static_cast<std::uint16_t>(100)));
    EXPECT_TRUE(allowlist.contains(static_cast<std::uint16_t>(42)));
    EXPECT_TRUE(allowlist.contains(static_cast<std::uint16_t>(1)));
    EXPECT_FALSE(allowlist.contains(static_cast<std::uint16_t>(0)));
    EXPECT_FALSE(allowlist.contains(static_cast<std::uint16_t>(65535)));
}

TEST(RudpRolloutPolicyTest, ParseUdpAllowlistMatchesRudpParserSemantics) {
    const auto udp_allowlist = server::core::realtime::parse_direct_opcode_allowlist("100, 0x2A,0X0001,invalid,,65536");

    EXPECT_EQ(udp_allowlist.size(), 3u);
    EXPECT_TRUE(udp_allowlist.contains(static_cast<std::uint16_t>(100)));
    EXPECT_TRUE(udp_allowlist.contains(static_cast<std::uint16_t>(42)));
    EXPECT_TRUE(udp_allowlist.contains(static_cast<std::uint16_t>(1)));
    EXPECT_FALSE(udp_allowlist.contains(static_cast<std::uint16_t>(0)));
    EXPECT_FALSE(udp_allowlist.contains(static_cast<std::uint16_t>(65535)));
}

TEST(RudpRolloutPolicyTest, ParseUdpAllowlistEmptyInputReturnsEmptySet) {
    const auto udp_allowlist = server::core::realtime::parse_direct_opcode_allowlist("  ,  ,\t");
    EXPECT_TRUE(udp_allowlist.empty());
}

TEST(RudpRolloutPolicyTest, SessionSelectionRespectsCanaryPercent) {
    server::core::realtime::DirectTransportRolloutPolicy policy;
    policy.enabled = true;

    policy.canary_percent = 0;
    EXPECT_FALSE(policy.session_selected("session-a", 101));

    policy.canary_percent = 100;
    EXPECT_TRUE(policy.session_selected("session-a", 101));

    policy.canary_percent = 25;
    const bool first = policy.session_selected("session-a", 101);
    const bool second = policy.session_selected("session-a", 101);
    EXPECT_EQ(first, second);
}

TEST(RudpRolloutPolicyTest, OpcodeAllowedRequiresAllowlistMembership) {
    server::core::realtime::DirectTransportRolloutPolicy policy;
    policy.enabled = true;
    policy.canary_percent = 100;
    policy.opcode_allowlist = {10, 11};

    EXPECT_TRUE(policy.opcode_allowed(10));
    EXPECT_TRUE(policy.opcode_allowed(11));
    EXPECT_FALSE(policy.opcode_allowed(12));
}

TEST(RudpRolloutPolicyTest, OpcodeAllowedRejectsWhenAllowlistIsEmpty) {
    server::core::realtime::DirectTransportRolloutPolicy policy;
    policy.enabled = true;
    policy.canary_percent = 100;
    policy.opcode_allowlist.clear();

    EXPECT_FALSE(policy.opcode_allowed(10));
}

TEST(RudpRolloutPolicyTest, AttachDecisionExplainsRolloutOutcome) {
    server::core::realtime::DirectTransportRolloutPolicy disabled_policy;
    const auto disabled = server::core::realtime::evaluate_direct_attach(disabled_policy, "session-a", 101);
    EXPECT_EQ(disabled.mode, server::core::realtime::DirectAttachMode::kUdpOnly);
    EXPECT_EQ(disabled.reason, server::core::realtime::DirectAttachReason::kRolloutDisabled);

    server::core::realtime::DirectTransportRolloutPolicy selected_policy;
    selected_policy.enabled = true;
    selected_policy.canary_percent = 100;
    selected_policy.opcode_allowlist = {10};
    const auto selected = server::core::realtime::evaluate_direct_attach(selected_policy, "session-a", 101);
    EXPECT_EQ(selected.mode, server::core::realtime::DirectAttachMode::kRudpCanary);
    EXPECT_EQ(selected.reason, server::core::realtime::DirectAttachReason::kCanarySelected);
}
