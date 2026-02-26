#include "server/core/concurrent/job_queue.hpp"
#include "server/core/runtime_metrics.hpp"

#include <chrono>

/**
 * @brief JobQueue(FIFO)의 동기화/백프레셔 구현입니다.
 *
 * 생산자-소비자 간 경합을 줄이면서 큐 포화 시 대기/거절 정책을 명확히 하여,
 * 과부하에서 메모리 폭주 대신 제어 가능한 지연으로 수렴시키는 것이 목적입니다.
 */
namespace server::core {

// JobQueue는 생산자 스레드가 DB/백그라운드 작업을 밀어 넣고,
// 워커 스레드가 Pop()으로 끌어가는 간단한 FIFO 구조입니다.
// mutex와 condition_variable을 사용하여 스레드 안전하게 구현되었습니다.

JobQueue::JobQueue(std::size_t max_size)
    : max_size_(max_size) {
    runtime_metrics::register_job_queue_capacity(max_size_);
}

void JobQueue::Push(Job job) {
    if (!job) {
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }

        if (max_size_ > 0 && jobs_.size() >= max_size_) {
            const auto wait_begin = std::chrono::steady_clock::now();
            cv_.wait(lock, [this] { return stopping_ || jobs_.size() < max_size_; });
            const auto waited = std::chrono::steady_clock::now() - wait_begin;
            runtime_metrics::record_job_queue_push_wait(waited);
            if (stopping_) {
                return;
            }
        }

        jobs_.push(std::move(job));
        // 메트릭 기록: 현재 큐 깊이
        runtime_metrics::record_job_queue_depth(jobs_.size());
    }
    // 대기 중인 워커 스레드 하나를 깨웁니다.
    cv_.notify_one();
}

bool JobQueue::TryPush(Job job) {
    if (!job) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        if (max_size_ > 0 && jobs_.size() >= max_size_) {
            runtime_metrics::record_job_queue_reject();
            runtime_metrics::record_job_queue_depth(jobs_.size());
            return false;
        }

        jobs_.push(std::move(job));
        runtime_metrics::record_job_queue_depth(jobs_.size());
    }
    cv_.notify_one();
    return true;
}

Job JobQueue::Pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    // 큐에 작업이 들어오거나 종료 신호가 올 때까지 대기합니다.
    cv_.wait(lock, [this] { return !jobs_.empty() || stopping_; });

    if (stopping_ && jobs_.empty()) {
        runtime_metrics::record_job_queue_depth(jobs_.size());
        return nullptr; // nullptr이면 종료 신호로 간주합니다.
    }

    Job job = std::move(jobs_.front());
    jobs_.pop();
    runtime_metrics::record_job_queue_depth(jobs_.size());
    lock.unlock();
    // 큐가 bounded 상태라면 대기 중인 생산자를 깨웁니다.
    if (max_size_ > 0) {
        cv_.notify_one();
    }
    return job;
}

void JobQueue::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        runtime_metrics::record_job_queue_depth(jobs_.size());
    }
    cv_.notify_all();
}

std::size_t JobQueue::max_size() const {
    return max_size_;
}

} // namespace server::core
