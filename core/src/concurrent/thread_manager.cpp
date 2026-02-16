#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/concurrent/job_queue.hpp"

/**
 * @brief JobQueue 소비용 고정 워커 스레드 풀 구현입니다.
 *
 * 작업마다 스레드를 만들지 않고 재사용해 생성/파괴 오버헤드를 줄이며,
 * 종료 시점에는 queue stop 신호로 모든 워커를 질서 있게 수렴시킵니다.
 */
namespace server::core {

ThreadManager::ThreadManager(JobQueue& job_queue)
    : job_queue_(job_queue) {}

ThreadManager::~ThreadManager() {
    Stop();
}

// ThreadManager는 JobQueue를 소비하는 고정 스레드 풀입니다.
// 각 워커 스레드는 JobQueue::Pop()이 nullptr를 반환할 때까지 반복 실행됩니다.
// 이를 통해 스레드 생성/삭제 오버헤드를 줄이고 안정적인 작업 처리를 보장합니다.
void ThreadManager::Start(int num_threads) {
    stopped_.store(false, std::memory_order_relaxed);
    threads_.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads_.emplace_back([this] { WorkerLoop(); });
    }
}

void ThreadManager::Stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    job_queue_.Stop(); // Pop()에서 대기 중인 워커들을 모두 깨워 종료 신호를 전달합니다.

    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
}

// 각 워커 스레드는 JobQueue::Pop()이 nullptr를 반환할 때까지 반복 실행됩니다.
void ThreadManager::WorkerLoop() {
    while (!stopped_.load(std::memory_order_acquire)) {
        Job job = job_queue_.Pop();
        if (!job) { // nullptr 작업이 오면 종료합니다.
            break;
        }
        job();
    }
}

} // namespace server::core
