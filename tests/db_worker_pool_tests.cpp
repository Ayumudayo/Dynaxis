#include <atomic>
#include <chrono>
#include <future>
#include <random>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "server/core/runtime_metrics.hpp"
#include "server/core/storage_execution/db_worker_pool.hpp"
#include "server/core/storage_execution/retry_backoff.hpp"

using namespace std::chrono_literals;
using server::core::runtime_metrics::snapshot;
using server::core::storage_execution::DbWorkerPool;
using server::core::storage_execution::IConnectionPool;
using server::core::storage_execution::IUnitOfWork;

/**
 * @brief DbWorkerPool의 커밋/롤백/실패 메트릭 반영 동작을 검증합니다.
 */
namespace {

struct FakeUnitState {
    std::atomic<int> commit_calls{0};
    std::atomic<int> rollback_calls{0};
};

class FakeUnitOfWork final : public IUnitOfWork {
public:
    explicit FakeUnitOfWork(std::shared_ptr<FakeUnitState> state)
        : state_(std::move(state)) {}

    void commit() override { state_->commit_calls.fetch_add(1); }
    void rollback() override { state_->rollback_calls.fetch_add(1); }

private:
    std::shared_ptr<FakeUnitState> state_;
};

class FakeConnectionPool final : public IConnectionPool {
public:
    explicit FakeConnectionPool(std::shared_ptr<FakeUnitState> state)
        : state_(std::move(state)) {}

    std::unique_ptr<IUnitOfWork> make_unit_of_work() override {
        return std::make_unique<FakeUnitOfWork>(state_);
    }

    bool health_check() override { return true; }

private:
    std::shared_ptr<FakeUnitState> state_;
};

// DB 워커 풀 테스트를 위한 Fake 구현체들입니다.
// 실제 DB 연결 없이 UnitOfWork의 커밋/롤백 동작을 검증하기 위해 사용합니다.
} // namespace

// 자동 커밋(Auto-Commit) 모드에서 작업이 성공하면 커밋이 호출되는지 확인합니다.
TEST(DbWorkerPoolTests, ProcessesJobWithAutoCommit) {
    auto state = std::make_shared<FakeUnitState>();
    auto pool = std::make_shared<FakeConnectionPool>(state);
    DbWorkerPool workers(pool);

    auto before = snapshot();
    workers.start(1); // 워커 스레드 1개 시작

    std::promise<void> completion;
    auto future = completion.get_future();
    // auto_commit = true로 작업 제출
    workers.submit([&](IUnitOfWork&) { completion.set_value(); }, true);

    ASSERT_EQ(future.wait_for(200ms), std::future_status::ready);

    workers.stop();

    // 커밋 호출 횟수 확인
    EXPECT_EQ(state->commit_calls.load(), 1);
    EXPECT_EQ(state->rollback_calls.load(), 0);

    // 메트릭 업데이트 확인
    auto after = snapshot();
    EXPECT_EQ(after.db_job_processed_total, before.db_job_processed_total + 1);
    EXPECT_EQ(after.db_job_failed_total, before.db_job_failed_total);
    EXPECT_EQ(after.db_job_queue_depth, 0u);
}

// 자동 커밋을 끄면(manual commit), 작업이 성공해도 커밋이 호출되지 않고 롤백됩니다.
// (작업 내부에서 명시적으로 커밋하지 않았을 경우 안전을 위해 롤백됨)
TEST(DbWorkerPoolTests, ProcessesJobWithoutAutoCommit) {
    auto state = std::make_shared<FakeUnitState>();
    auto pool = std::make_shared<FakeConnectionPool>(state);
    DbWorkerPool workers(pool);

    auto before = snapshot();
    workers.start(1);

    std::promise<void> completion;
    auto future = completion.get_future();
    // auto_commit = false로 작업 제출
    workers.submit([&](IUnitOfWork&) { completion.set_value(); }, false);

    ASSERT_EQ(future.wait_for(200ms), std::future_status::ready);

    workers.stop();

    // 커밋 0회, 롤백 1회 예상
    EXPECT_EQ(state->commit_calls.load(), 0);
    EXPECT_EQ(state->rollback_calls.load(), 1);

    auto after = snapshot();
    EXPECT_EQ(after.db_job_processed_total, before.db_job_processed_total + 1);
    EXPECT_EQ(after.db_job_failed_total, before.db_job_failed_total);
}

// 작업 수행 중 예외가 발생하면 자동으로 롤백되고 실패 메트릭이 증가하는지 확인합니다.
TEST(DbWorkerPoolTests, JobExceptionTriggersRollbackAndFailureMetric) {
    auto state = std::make_shared<FakeUnitState>();
    auto pool = std::make_shared<FakeConnectionPool>(state);
    DbWorkerPool workers(pool);

    auto before = snapshot();
    workers.start(1);

    std::atomic<bool> invoked{false};
    workers.submit([&](IUnitOfWork&) {
        invoked.store(true);
        throw std::runtime_error("fail");
    });

    // 작업이 처리될 때까지 대기
    for (int i = 0; i < 50 && !invoked.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    std::this_thread::sleep_for(20ms);
    workers.stop();

    // 롤백 호출 확인
    EXPECT_GE(state->rollback_calls.load(), 1);
    EXPECT_EQ(state->commit_calls.load(), 0);

    // 실패 메트릭 증가 확인
    auto after = snapshot();
    EXPECT_EQ(after.db_job_failed_total, before.db_job_failed_total + 1);
    EXPECT_EQ(after.db_job_processed_total, before.db_job_processed_total);
}

TEST(DbWorkerPoolTests, SubmitThrowsWhenNotRunning) {
    auto state = std::make_shared<FakeUnitState>();
    auto pool = std::make_shared<FakeConnectionPool>(state);
    DbWorkerPool workers(pool);

    EXPECT_THROW(workers.submit([](IUnitOfWork&) {}, true), std::runtime_error);
}

TEST(DbWorkerPoolTests, RetryBackoffLinearModeUsesExactScaledDelay) {
    const auto delay = server::core::storage_execution::retry_backoff_upper_bound_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kLinear,
            .base_delay_ms = 250,
            .max_delay_ms = 1'000,
        },
        3);

    EXPECT_EQ(delay, 750u);
}

TEST(DbWorkerPoolTests, RetryBackoffExponentialFullJitterCapsUpperBoundAndSamplesWithinRange) {
    const auto upper_bound = server::core::storage_execution::retry_backoff_upper_bound_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kExponentialFullJitter,
            .base_delay_ms = 500,
            .max_delay_ms = 2'000,
        },
        3);
    EXPECT_EQ(upper_bound, 2'000u);

    std::mt19937_64 rng(13);
    const auto sampled = server::core::storage_execution::sample_retry_backoff_delay_ms(
        server::core::storage_execution::RetryBackoffPolicy{
            .mode = server::core::storage_execution::RetryBackoffMode::kExponentialFullJitter,
            .base_delay_ms = 500,
            .max_delay_ms = 2'000,
        },
        3,
        rng);

    EXPECT_LE(sampled, upper_bound);
}
