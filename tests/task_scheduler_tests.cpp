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
