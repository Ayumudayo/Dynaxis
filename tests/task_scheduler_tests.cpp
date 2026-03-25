#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "server/core/concurrent/task_scheduler.hpp"

using namespace std::chrono_literals;
using server::core::concurrent::TaskScheduler;

/**
 * @brief TaskScheduler 즉시/지연/주기 실행 및 shutdown 동작을 검증합니다.
 */
// 즉시 실행 작업(post)이 순서대로 실행되는지 확인합니다.
TEST(TaskSchedulerTests, PostExecutesInOrder) {
    TaskScheduler scheduler;
    std::vector<int> results;

    scheduler.post([&]() { results.push_back(1); });
    scheduler.post([&]() { results.push_back(2); });
    scheduler.post([&]() { results.push_back(3); });

    auto executed = scheduler.poll();

    ASSERT_EQ(executed, 3u);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);
    EXPECT_EQ(results[2], 3);
}

// 지연 실행 작업(schedule)이 지정된 시간이 지난 후에만 실행되는지 확인합니다.
TEST(TaskSchedulerTests, DelayedTasksExecuteAfterDelay) {
    TaskScheduler scheduler;
    std::atomic<int> counter{0};

    scheduler.schedule([&]() { counter.fetch_add(1); }, 20ms);

    // 아직 시간이 안 지났으므로 실행되지 않음
    EXPECT_EQ(scheduler.poll(), 0u);
    std::this_thread::sleep_for(30ms);
    // 시간이 지났으므로 실행됨
    EXPECT_EQ(scheduler.poll(), 1u);
    EXPECT_EQ(counter.load(), 1);
}

// 반복 실행 작업(schedule_every)이 주기적으로 실행되는지 확인합니다.
TEST(TaskSchedulerTests, ScheduleEveryRepeatsUntilShutdown) {
    TaskScheduler scheduler;
    std::atomic<int> counter{0};

    scheduler.schedule_every([&]() { counter.fetch_add(1); }, 5ms);

    auto start = TaskScheduler::Clock::now();
    while (TaskScheduler::Clock::now() - start < 80ms) {
        scheduler.poll();
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_GE(counter.load(), 3);

    scheduler.shutdown();
    auto before = counter.load();
    std::this_thread::sleep_for(20ms);
    scheduler.poll();
    // 셧다운 후에는 더 이상 실행되지 않음
    EXPECT_EQ(counter.load(), before);
    EXPECT_TRUE(scheduler.empty());
}

TEST(TaskSchedulerTests, PollHonorsMaxTasks) {
    TaskScheduler scheduler;
    std::vector<int> order;

    scheduler.post([&]() { order.push_back(1); });
    scheduler.post([&]() { order.push_back(2); });
    scheduler.post([&]() { order.push_back(3); });

    auto first = scheduler.poll(2);
    EXPECT_EQ(first, 2u);
    EXPECT_EQ(order.size(), 2u);

    auto second = scheduler.poll(1);
    EXPECT_EQ(second, 1u);
    EXPECT_EQ(order.size(), 3u);
}

TEST(TaskSchedulerTests, ShutdownClearsPendingTasks) {
    TaskScheduler scheduler;
    std::atomic<int> counter{0};

    scheduler.post([&]() { counter.fetch_add(1); });
    scheduler.schedule([&]() { counter.fetch_add(1); }, 1ms);

    scheduler.shutdown();

    EXPECT_TRUE(scheduler.empty());
    EXPECT_EQ(scheduler.poll(), 0u);
    EXPECT_EQ(counter.load(), 0);
}

TEST(TaskSchedulerTests, ControlledTasksCanBeCanceledByTokenAndGroup) {
    TaskScheduler scheduler;
    std::atomic<int> counter{0};

    const auto cancel_group = scheduler.create_cancel_group();
    const auto token_a = scheduler.schedule_controlled(
        [&]() { counter.fetch_add(1, std::memory_order_relaxed); },
        5ms,
        TaskScheduler::TaskOptions{.cancel_group = cancel_group}).cancel_token;
    const auto token_b = scheduler.schedule_controlled(
        [&]() { counter.fetch_add(10, std::memory_order_relaxed); },
        5ms,
        TaskScheduler::TaskOptions{.cancel_group = cancel_group}).cancel_token;
    const auto standalone = scheduler.schedule_controlled(
        [&]() { counter.fetch_add(100, std::memory_order_relaxed); },
        5ms);
    ASSERT_TRUE(standalone);

    EXPECT_TRUE(scheduler.cancel(token_a));
    EXPECT_EQ(scheduler.cancel(cancel_group), 1u);

    std::this_thread::sleep_for(15ms);
    EXPECT_EQ(scheduler.poll(), 1u);
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 100);
}

TEST(TaskSchedulerTests, ControlledTaskCanBeRescheduledAndValidatorCanGateExecution) {
    TaskScheduler scheduler;
    std::atomic<int> counter{0};
    std::atomic<bool> validator_open{false};

    const auto gated = scheduler.schedule_controlled(
        [&]() { counter.fetch_add(1, std::memory_order_relaxed); },
        5ms,
        TaskScheduler::TaskOptions{
            .validator = [&]() { return validator_open.load(std::memory_order_relaxed); },
        });
    const auto rescheduled = scheduler.schedule_controlled(
        [&]() { counter.fetch_add(10, std::memory_order_relaxed); },
        5ms);

    EXPECT_TRUE(scheduler.reschedule(rescheduled.cancel_token, 40ms));

    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(scheduler.poll(), 0u);
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 0);

    validator_open.store(true, std::memory_order_relaxed);
    std::this_thread::sleep_for(40ms);
    EXPECT_EQ(scheduler.poll(), 1u);
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 10);
    EXPECT_FALSE(scheduler.cancel(gated.cancel_token));
}

TEST(TaskSchedulerTests, ControlledRepeatTasksExposeContextAndSupportPolicyUpdate) {
    TaskScheduler scheduler;
    std::vector<std::size_t> run_counts;
    std::vector<std::chrono::milliseconds> intervals;

    const auto handle = scheduler.schedule_every_controlled(
        [&](const TaskScheduler::RepeatContext& context) {
            run_counts.push_back(context.run_count);
            intervals.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(context.current_interval));
            return run_counts.size() >= 2
                ? TaskScheduler::RepeatDecision::kStop
                : TaskScheduler::RepeatDecision::kContinue;
        },
        TaskScheduler::RepeatPolicy{
            .interval = 5ms,
        });

    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(scheduler.poll(), 1u);
    EXPECT_TRUE(scheduler.update_repeat_policy(
        handle.cancel_token,
        TaskScheduler::RepeatPolicy{
            .interval = 1ms,
        }));

    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(scheduler.poll(), 1u);

    ASSERT_EQ(run_counts.size(), 2u);
    EXPECT_EQ(run_counts[0], 0u);
    EXPECT_EQ(run_counts[1], 1u);
    EXPECT_EQ(intervals[0], 5ms);
    EXPECT_EQ(intervals[1], 1ms);
}

TEST(TaskSchedulerTests, ControlledRepeatTasksTrackValidatorSkipsAndBackoff) {
    TaskScheduler scheduler;
    std::atomic<bool> gate_open{false};
    std::vector<std::size_t> skip_counts;
    std::vector<std::chrono::milliseconds> intervals;

    const auto handle = scheduler.schedule_every_controlled(
        [&](const TaskScheduler::RepeatContext& context) {
            skip_counts.push_back(context.validator_skip_count);
            intervals.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(context.current_interval));
            return TaskScheduler::RepeatDecision::kStop;
        },
        TaskScheduler::RepeatPolicy{
            .interval = 5ms,
            .max_interval = 20ms,
            .backoff_multiplier = 2.0,
            .jitter = 0ms,
        },
        [&](const TaskScheduler::RepeatContext&) {
            return gate_open.load(std::memory_order_relaxed);
        });
    ASSERT_TRUE(handle);

    std::this_thread::sleep_for(8ms);
    EXPECT_EQ(scheduler.poll(), 0u);

    gate_open.store(true, std::memory_order_relaxed);
    std::this_thread::sleep_for(12ms);
    EXPECT_EQ(scheduler.poll(), 1u);

    ASSERT_EQ(skip_counts.size(), 1u);
    EXPECT_EQ(skip_counts[0], 1u);
    EXPECT_EQ(intervals[0], 10ms);
}

TEST(TaskSchedulerTests, ControlledRepeatPolicyUpdateRejectsNonPositiveIntervals) {
    TaskScheduler scheduler;
    std::vector<std::chrono::milliseconds> intervals;

    const auto handle = scheduler.schedule_every_controlled(
        [&](const TaskScheduler::RepeatContext& context) {
            intervals.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(context.current_interval));
            return TaskScheduler::RepeatDecision::kStop;
        },
        TaskScheduler::RepeatPolicy{
            .interval = 20ms,
        });
    ASSERT_TRUE(handle);

    EXPECT_FALSE(scheduler.update_repeat_policy(
        handle.cancel_token,
        TaskScheduler::RepeatPolicy{
            .interval = 0ms,
        }));
    EXPECT_FALSE(scheduler.update_repeat_policy(
        handle.cancel_token,
        TaskScheduler::RepeatPolicy{
            .interval = -5ms,
        }));

    std::this_thread::sleep_for(25ms);
    EXPECT_EQ(scheduler.poll(), 1u);

    ASSERT_EQ(intervals.size(), 1u);
    EXPECT_EQ(intervals.front(), 20ms);
}
