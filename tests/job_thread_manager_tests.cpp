#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/thread_manager.hpp"

using namespace std::chrono_literals;
using server::core::Job;
using server::core::JobQueue;
using server::core::ThreadManager;

/**
 * @brief JobQueue/ThreadManager의 대기/깨움/실행/정지 동작을 검증합니다.
 */
// 큐가 비어있을 때 Pop()이 블로킹(대기)되는지 확인합니다.
TEST(JobQueueTests, PopBlocksUntilJobArrives) {
    JobQueue queue;
    std::atomic<int> counter{0};
    std::promise<void> ready;

    std::thread worker([&]() {
        ready.set_value();
        // 큐에 작업이 들어올 때까지 대기
        Job job = queue.Pop();
        ASSERT_TRUE(job);
        job();
    });

    ready.get_future().wait();
    // 작업 추가
    queue.Push([&]() { counter.fetch_add(1); });
    worker.join();

    EXPECT_EQ(counter.load(), 1);
}

// Stop() 호출 시 대기 중인 소비자 스레드가 깨어나는지 확인합니다.
TEST(JobQueueTests, StopWakesWaitingConsumer) {
    JobQueue queue;
    std::promise<void> ready;
    std::atomic<bool> woke{false};

    std::thread waiter([&]() {
        ready.set_value();
        // 큐가 멈추면 빈 Job을 반환하며 리턴
        Job job = queue.Pop();
        EXPECT_FALSE(job);
        woke.store(true);
    });

    ready.get_future().wait();
    std::this_thread::sleep_for(5ms);
    queue.Stop();

    waiter.join();
    EXPECT_TRUE(woke.load());
}

// ThreadManager가 큐에 쌓인 작업을 여러 스레드에서 처리하는지 확인합니다.
TEST(ThreadManagerTests, ExecutesQueuedJobs) {
    JobQueue queue;
    ThreadManager manager(queue);

    manager.Start(2); // 2개의 워커 스레드 시작
    std::atomic<int> completed{0};
    for (int i = 0; i < 6; ++i) {
        queue.Push([&completed]() { completed.fetch_add(1); });
    }

    // 작업 완료 대기
    for (int i = 0; i < 100 && completed.load() < 6; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    manager.Stop();
    EXPECT_EQ(completed.load(), 6);
}

TEST(ThreadManagerTests, StopIsIdempotent) {
    JobQueue queue;
    ThreadManager manager(queue);

    manager.Start(1);
    queue.Push([]() {});

    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(1ms);
    }

    manager.Stop();
    manager.Stop();
    SUCCEED();
}
