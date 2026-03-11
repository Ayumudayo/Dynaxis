#include <gtest/gtest.h>

#include <gateway/resilience_controls.hpp>

TEST(GatewayTokenBucketTest, EnforcesBurstAndRefillRate) {
    gateway::TokenBucket bucket;
    bucket.configure(2.0, 4.0);

    EXPECT_TRUE(bucket.consume(1000));
    EXPECT_TRUE(bucket.consume(1000));
    EXPECT_TRUE(bucket.consume(1000));
    EXPECT_TRUE(bucket.consume(1000));
    EXPECT_FALSE(bucket.consume(1000));

    // 500ms 경과 시 1 token(2/sec * 0.5s)만큼 refill 된다.
    EXPECT_TRUE(bucket.consume(1500));
    EXPECT_FALSE(bucket.consume(1500));
}

TEST(GatewayTokenBucketTest, AvailableClampsToBurstAfterLongIdle) {
    gateway::TokenBucket bucket;
    bucket.configure(5.0, 10.0);

    EXPECT_TRUE(bucket.consume(1000, 7.0));
    EXPECT_NEAR(bucket.available(1000), 3.0, 1e-9);
    EXPECT_NEAR(bucket.available(5000), 10.0, 1e-9);
}

TEST(GatewayRetryBudgetTest, ResetsBudgetAfterWindow) {
    gateway::RetryBudget budget;
    budget.configure(3, 1000);

    EXPECT_TRUE(budget.consume(100));
    EXPECT_TRUE(budget.consume(200));
    EXPECT_TRUE(budget.consume(300));
    EXPECT_FALSE(budget.consume(900));

    EXPECT_TRUE(budget.consume(1101));
}

TEST(GatewayRetryBudgetTest, RemainingTracksBudgetAndClockRollbackResetsWindow) {
    gateway::RetryBudget budget;
    budget.configure(2, 1000);

    EXPECT_EQ(budget.remaining(100), 2u);
    EXPECT_TRUE(budget.consume(100));
    EXPECT_EQ(budget.remaining(100), 1u);
    EXPECT_TRUE(budget.consume(200));
    EXPECT_EQ(budget.remaining(200), 0u);
    EXPECT_FALSE(budget.consume(300));

    EXPECT_EQ(budget.remaining(50), 2u);
    EXPECT_TRUE(budget.consume(50));
}

TEST(GatewayCircuitBreakerTest, OpensAfterThresholdAndRecoversAfterWindow) {
    gateway::CircuitBreaker breaker;
    breaker.configure(true, 3, 500);

    EXPECT_TRUE(breaker.allow(100));
    EXPECT_FALSE(breaker.record_failure(101));
    EXPECT_FALSE(breaker.record_failure(102));
    EXPECT_TRUE(breaker.record_failure(103));

    EXPECT_TRUE(breaker.is_open(200));
    EXPECT_FALSE(breaker.allow(200));

    EXPECT_FALSE(breaker.is_open(700));
    EXPECT_TRUE(breaker.allow(700));

    breaker.record_success();
    EXPECT_FALSE(breaker.is_open(701));
}

TEST(GatewayCircuitBreakerTest, DisabledBreakerNeverOpensOrRejects) {
    gateway::CircuitBreaker breaker;
    breaker.configure(false, 1, 500);

    EXPECT_TRUE(breaker.allow(100));
    EXPECT_FALSE(breaker.record_failure(101));
    EXPECT_FALSE(breaker.is_open(101));

    breaker.record_success();
    EXPECT_TRUE(breaker.allow(102));
}
