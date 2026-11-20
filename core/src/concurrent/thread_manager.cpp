#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"

#include <stdexcept>

/**
 * @brief JobQueue 소비용 고정 워커 스레드 풀 구현입니다.
 *
 * 작업마다 스레드를 만들지 않고 재사용해 생성/파괴 오버헤드를 줄이며,
 * 종료 시점에는 queue stop 신호로 모든 워커를 질서 있게 수렴시킵니다.
 */
namespace server::core {
namespace {
thread_local const ThreadManager* current_worker_manager = nullptr;
}

ThreadManager::ThreadManager(JobQueue& job_queue)
    : job_queue_(job_queue) {}

ThreadManager::~ThreadManager() {
    Stop();
}

// ThreadManager는 JobQueue를 소비하는 고정 스레드 풀이다.
// 각 워커 스레드는 `JobQueue::Pop()`이 nullptr를 반환할 때까지 반복 실행된다.
// "작업마다 새 스레드 생성" 방식을 피해야 생성/파괴 오버헤드와 종료 순서 혼란을 줄일 수 있다.
void ThreadManager::Start(int num_threads) {
    if (num_threads <= 0) {
        return;
    }

    bool expected = true;
    if (!stopped_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;
    }

    threads_.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads_.emplace_back([this] { WorkerLoop(); });
    }
}

void ThreadManager::Stop() {
    if (current_worker_manager == this) {
        throw std::logic_error("ThreadManager::Stop must be called outside its workers");
    }
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    job_queue_.Stop(); // `Pop()`에서 대기 중인 워커들을 모두 깨워 종료 신호를 전달한다.

    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
}

// 각 워커 스레드는 `JobQueue::Pop()`이 nullptr를 반환할 때까지 반복 실행된다.
// 종료 신호를 별도 채널로 두지 않고 queue stop에 수렴시키면, 생산자/소비자 종료 규약을 한곳에 모을 수 있다.
void ThreadManager::WorkerLoop() {
    current_worker_manager = this;
    for (;;) {
        Job job = job_queue_.Pop();
        if (!job) { // nullptr 작업이 오면 종료합니다.
            break;
        }
        try {
            job();
        } catch (...) {
            runtime_metrics::record_exception_recoverable();
            try { log::error("component=thread_manager job threw; continuing worker"); } catch (...) {}
        }
    }
    current_worker_manager = nullptr;
}

} // namespace server::core
