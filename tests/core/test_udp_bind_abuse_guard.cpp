#include <gtest/gtest.h>

#include <gateway/udp_bind_abuse_guard.hpp>

TEST(UdpBindAbuseGuardTest, BlocksAfterConfiguredFailureLimit) {
    gateway::UdpBindAbuseGuard guard;
    guard.configure(10000, 3, 2000);

    const std::string endpoint = "127.0.0.1:7777";

    EXPECT_FALSE(guard.record_failure(endpoint, 100));
    EXPECT_FALSE(guard.record_failure(endpoint, 200));
    EXPECT_TRUE(guard.record_failure(endpoint, 300));

    const auto blocked = guard.block_state(endpoint, 301);
    EXPECT_TRUE(blocked.blocked);
    EXPECT_GT(blocked.retry_after_ms, 0);
}

TEST(UdpBindAbuseGuardTest, UnblocksAfterBlockWindowExpires) {
    gateway::UdpBindAbuseGuard guard;
    guard.configure(10000, 2, 1000);

    const std::string endpoint = "127.0.0.1:8888";

    EXPECT_FALSE(guard.record_failure(endpoint, 100));
    EXPECT_TRUE(guard.record_failure(endpoint, 200));

    EXPECT_TRUE(guard.block_state(endpoint, 500).blocked);
    EXPECT_FALSE(guard.block_state(endpoint, 1201).blocked);
}

TEST(UdpBindAbuseGuardTest, ResetsFailureWindowOnSuccess) {
    gateway::UdpBindAbuseGuard guard;
    guard.configure(10000, 2, 1000);

    const std::string endpoint = "127.0.0.1:9999";

    EXPECT_FALSE(guard.record_failure(endpoint, 100));
    guard.record_success(endpoint);
    EXPECT_FALSE(guard.record_failure(endpoint, 200));
    EXPECT_TRUE(guard.record_failure(endpoint, 300));
}

TEST(UdpBindAbuseGuardTest, FailureWindowExpirationPreventsAccumulation) {
    gateway::UdpBindAbuseGuard guard;
    guard.configure(500, 2, 1000);

    const std::string endpoint = "127.0.0.1:10000";

    EXPECT_FALSE(guard.record_failure(endpoint, 100));
    EXPECT_FALSE(guard.record_failure(endpoint, 800));
    EXPECT_TRUE(guard.record_failure(endpoint, 900));
}

TEST(UdpBindAbuseGuardTest, EndpointIsolationMitigatesCrossEndpointHijackNoise) {
    gateway::UdpBindAbuseGuard guard;
    guard.configure(10000, 2, 3000);

    const std::string offender = "10.10.10.10:40000";
    const std::string normal = "10.10.10.11:40001";

    EXPECT_FALSE(guard.record_failure(offender, 100));
    EXPECT_TRUE(guard.record_failure(offender, 200));
    EXPECT_TRUE(guard.block_state(offender, 201).blocked);

    EXPECT_FALSE(guard.block_state(normal, 201).blocked);
    EXPECT_FALSE(guard.record_failure(normal, 210));
    EXPECT_FALSE(guard.block_state(normal, 220).blocked);
}
