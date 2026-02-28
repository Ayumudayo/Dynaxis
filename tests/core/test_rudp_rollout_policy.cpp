#include <gtest/gtest.h>

#include <gateway/rudp_rollout_policy.hpp>

#include <cstdint>

TEST(RudpRolloutPolicyTest, ParseAllowlistSupportsDecimalAndHexTokens) {
    const auto allowlist = gateway::parse_rudp_opcode_allowlist("100, 0x2A,0X0001,invalid,,65536");

    EXPECT_TRUE(allowlist.contains(static_cast<std::uint16_t>(100)));
    EXPECT_TRUE(allowlist.contains(static_cast<std::uint16_t>(42)));
    EXPECT_TRUE(allowlist.contains(static_cast<std::uint16_t>(1)));
    EXPECT_FALSE(allowlist.contains(static_cast<std::uint16_t>(65535)));
}

TEST(RudpRolloutPolicyTest, SessionSelectionRespectsCanaryPercent) {
    gateway::RudpRolloutPolicy policy;
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
    gateway::RudpRolloutPolicy policy;
    policy.enabled = true;
    policy.canary_percent = 100;
    policy.opcode_allowlist = {10, 11};

    EXPECT_TRUE(policy.opcode_allowed(10));
    EXPECT_TRUE(policy.opcode_allowed(11));
    EXPECT_FALSE(policy.opcode_allowed(12));
}
