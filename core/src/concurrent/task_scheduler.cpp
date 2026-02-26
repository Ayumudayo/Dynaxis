#include "server/core/concurrent/task_scheduler.hpp"

#include <limits>
#include <memory>

/**
 * @brief TaskScheduler의 지연/주기 작업 스케줄링 구현입니다.
 *
 * 별도 타이머 스레드 없이 `poll()` 기반 pull 모델을 사용해,
 * 이벤트 루프 틱 제어권을 호출자에게 남기면서 주기 작업을 안정적으로 수행합니다.
 */
namespace server::core::concurrent {

TaskScheduler::TaskScheduler() = default;

TaskScheduler::~TaskScheduler() {
    shutdown();
}

void TaskScheduler::shutdown() {
    if (shutdown_.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    while (!ready_.empty()) ready_.pop();
    while (!delayed_.empty()) delayed_.pop();
}

bool TaskScheduler::is_shutdown() const {
    return shutdown_.load(std::memory_order_relaxed);
}

void TaskScheduler::post(Task task) {
    if (!task || is_shutdown()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ready_.push(std::move(task));
}

void TaskScheduler::schedule(Task task, Clock::duration delay) {
    if (!task || is_shutdown()) {
        return;
    }
    if (delay <= Clock::duration::zero()) {
        post(std::move(task));
        return;
    }
    // 지연 작업은 우선순위 큐(top = 가장 빠른 due)로 관리됩니다.
    // poll() 호출 시점에 시간이 된 작업들을 ready 큐로 옮깁니다.
    // 이렇게 함으로써 별도의 타이머 스레드 없이도 지연 실행을 지원합니다.
    std::lock_guard<std::mutex> lock(mutex_);
    delayed_.push(DelayedTask{Clock::now() + delay, std::move(task)});
}

void TaskScheduler::schedule_every(Task task, Clock::duration interval) {
    if (!task || interval <= Clock::duration::zero()) {
        post(std::move(task));
        return;
    }

    // 재귀적으로 자신을 스케줄링하여 주기적인 실행을 구현합니다.
    schedule([this, interval, task = std::move(task)]() mutable {
        if (is_shutdown()) return;
        task();
        // 주기 작업은 자기 자신을 다시 enqueue하여 close/shutdown까지 반복합니다.
        schedule_every(task, interval);
    }, interval);
}

std::size_t TaskScheduler::poll(std::size_t max_tasks) {
    if (is_shutdown()) {
        return 0;
    }
    std::vector<Task> tasks;
    collect_ready(tasks, max_tasks == static_cast<std::size_t>(-1) ? std::numeric_limits<std::size_t>::max() : max_tasks);
    std::size_t executed = 0;
    for (auto& fn : tasks) {
        if (!fn) continue;
        fn();
        ++executed;
    }
    return executed;
}

void TaskScheduler::collect_ready(std::vector<Task>& out, std::size_t max_tasks) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = Clock::now();
    while (!delayed_.empty() && delayed_.top().due <= now) {
        ready_.push(std::move(delayed_.top().task));
        delayed_.pop();
    }
    while (!ready_.empty() && out.size() < max_tasks) {
        out.emplace_back(std::move(ready_.front()));
        ready_.pop();
    }
}

bool TaskScheduler::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_.empty() && delayed_.empty();
}

} // namespace server::core::concurrent
