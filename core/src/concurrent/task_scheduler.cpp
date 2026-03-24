#include "server/core/concurrent/task_scheduler.hpp"

#include <limits>

/**
 * @brief TaskScheduler의 지연/반복 작업 스케줄링 구현입니다.
 *
 * 별도 타이머 스레드 없이 `poll()` 기반 pull 모델을 유지하면서도,
 * cancel token/group, validator, reschedule/update, repeat context와
 * jitter/backoff 같은 richer control surface를 제공합니다.
 */
namespace server::core::concurrent {

struct TaskScheduler::TaskRecord {
    TaskId id{0};
    CancelGroup group{};
    Task one_shot{};
    RepeatTask repeat{};
    Validator validator{};
    RepeatValidator repeat_validator{};
    RepeatPolicy repeat_policy{};
    RepeatContext repeat_context{};
    bool repeating{false};
    bool canceled{false};
    std::uint64_t generation{0};
};

namespace {

TaskScheduler::Clock::duration clamp_duration_to_non_negative(TaskScheduler::Clock::duration delay) {
    return delay <= TaskScheduler::Clock::duration::zero()
        ? TaskScheduler::Clock::duration::zero()
        : delay;
}

bool is_valid_repeat_policy(const TaskScheduler::RepeatPolicy& policy) {
    return policy.interval > TaskScheduler::Clock::duration::zero();
}

} // namespace

TaskScheduler::TaskScheduler() = default;

TaskScheduler::~TaskScheduler() {
    shutdown();
}

void TaskScheduler::shutdown() {
    if (shutdown_.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.clear();
    while (!ready_.empty()) ready_.pop();
    while (!delayed_.empty()) delayed_.pop();
}

TaskScheduler::CancelGroup TaskScheduler::create_cancel_group() {
    return CancelGroup{next_group_id_.fetch_add(1, std::memory_order_relaxed)};
}

bool TaskScheduler::is_shutdown() const {
    return shutdown_.load(std::memory_order_relaxed);
}

TaskScheduler::ScheduleHandle TaskScheduler::make_handle(TaskId id, CancelGroup group) const noexcept {
    return ScheduleHandle{
        .task_id = id,
        .cancel_token = CancelToken{id},
        .cancel_group = group,
    };
}

TaskScheduler::Clock::duration TaskScheduler::apply_jitter(Clock::duration delay, Clock::duration jitter) {
    delay = clamp_duration_to_non_negative(delay);
    const auto jitter_cap = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clamp_duration_to_non_negative(jitter)).count();
    if (jitter_cap <= 0) {
        return delay;
    }
    std::uniform_int_distribution<long long> distribution(0, jitter_cap);
    return delay + std::chrono::nanoseconds(distribution(jitter_rng_));
}

TaskScheduler::Clock::duration TaskScheduler::next_repeat_delay(const TaskRecord& record) {
    auto delay = record.repeat_context.current_interval;
    if (delay <= Clock::duration::zero()) {
        delay = record.repeat_policy.interval;
    }

    if (record.repeat_policy.backoff_multiplier > 1.0) {
        const auto current_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
        const auto scaled = static_cast<long double>(current_ns) * record.repeat_policy.backoff_multiplier;
        auto next_ns = static_cast<long long>(scaled);
        if (next_ns < 0) {
            next_ns = 0;
        }

        if (record.repeat_policy.max_interval > Clock::duration::zero()) {
            const auto cap_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(record.repeat_policy.max_interval).count();
            next_ns = std::min(next_ns, cap_ns);
        }
        delay = std::chrono::nanoseconds(next_ns);
    } else {
        delay = record.repeat_policy.interval;
    }

    return apply_jitter(delay, record.repeat_policy.jitter);
}

void TaskScheduler::post(Task task) {
    (void)post_controlled(std::move(task));
}

TaskScheduler::ScheduleHandle TaskScheduler::post_controlled(Task task, TaskOptions options) {
    return schedule_controlled(std::move(task), Clock::duration::zero(), std::move(options));
}

void TaskScheduler::schedule(Task task, Clock::duration delay) {
    (void)schedule_controlled(std::move(task), delay);
}

TaskScheduler::ScheduleHandle TaskScheduler::schedule_controlled(Task task,
                                                                 Clock::duration delay,
                                                                 TaskOptions options) {
    if (!task || is_shutdown()) {
        return {};
    }

    auto record = std::make_shared<TaskRecord>();
    record->id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
    record->group = options.cancel_group;
    record->one_shot = std::move(task);
    record->validator = std::move(options.validator);

    const auto due = Clock::now() + clamp_duration_to_non_negative(delay);

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown()) {
        return {};
    }

    tasks_[record->id] = record;
    if (delay <= Clock::duration::zero()) {
        ready_.push(ReadyTask{record, record->generation});
    } else {
        delayed_.push(DelayedTask{due, record, record->generation});
    }
    return make_handle(record->id, record->group);
}

void TaskScheduler::schedule_every(Task task, Clock::duration interval) {
    RepeatPolicy policy;
    policy.interval = interval;
    (void)schedule_every_controlled(
        [task = std::move(task)](const RepeatContext&) mutable {
            task();
            return RepeatDecision::kContinue;
        },
        policy);
}

TaskScheduler::ScheduleHandle TaskScheduler::schedule_every_controlled(RepeatTask task,
                                                                       RepeatPolicy policy,
                                                                       RepeatValidator validator,
                                                                       CancelGroup cancel_group) {
    if (!task || !is_valid_repeat_policy(policy) || is_shutdown()) {
        return {};
    }

    auto record = std::make_shared<TaskRecord>();
    record->id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
    record->group = cancel_group;
    record->repeat = std::move(task);
    record->repeat_validator = std::move(validator);
    record->repeat_policy = std::move(policy);
    record->repeating = true;
    const auto initial_delay = apply_jitter(record->repeat_policy.interval, record->repeat_policy.jitter);
    record->repeat_context.current_interval = initial_delay;
    record->repeat_context.first_due = Clock::now() + initial_delay;
    record->repeat_context.last_due = record->repeat_context.first_due;

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown()) {
        return {};
    }

    tasks_[record->id] = record;
    delayed_.push(DelayedTask{record->repeat_context.first_due, record, record->generation});
    return make_handle(record->id, record->group);
}

bool TaskScheduler::cancel(CancelToken token) {
    if (!token || is_shutdown()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(token.value);
    if (it == tasks_.end()) {
        return false;
    }
    it->second->canceled = true;
    ++it->second->generation;
    tasks_.erase(it);
    return true;
}

std::size_t TaskScheduler::cancel(CancelGroup group) {
    if (!group || is_shutdown()) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t canceled = 0;
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->second && it->second->group == group) {
            it->second->canceled = true;
            ++it->second->generation;
            it = tasks_.erase(it);
            ++canceled;
            continue;
        }
        ++it;
    }
    return canceled;
}

bool TaskScheduler::reschedule(CancelToken token, Clock::duration delay) {
    if (!token || is_shutdown()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(token.value);
    if (it == tasks_.end() || !it->second || it->second->canceled) {
        return false;
    }

    auto& record = *it->second;
    ++record.generation;
    const auto next_due = Clock::now() + clamp_duration_to_non_negative(delay);
    if (delay <= Clock::duration::zero()) {
        ready_.push(ReadyTask{it->second, record.generation});
    } else {
        delayed_.push(DelayedTask{next_due, it->second, record.generation});
    }
    if (record.repeating) {
        record.repeat_context.current_interval = clamp_duration_to_non_negative(delay);
        record.repeat_context.last_due = next_due;
    }
    return true;
}

bool TaskScheduler::update_repeat_policy(CancelToken token, RepeatPolicy policy) {
    if (!token || !is_valid_repeat_policy(policy) || is_shutdown()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(token.value);
    if (it == tasks_.end() || !it->second || !it->second->repeating || it->second->canceled) {
        return false;
    }

    auto& record = *it->second;
    record.repeat_policy = std::move(policy);
    ++record.generation;
    const auto delay = apply_jitter(record.repeat_policy.interval, record.repeat_policy.jitter);
    const auto next_due = Clock::now() + delay;
    record.repeat_context.current_interval = delay;
    record.repeat_context.last_due = next_due;
    delayed_.push(DelayedTask{next_due, it->second, record.generation});
    return true;
}

std::size_t TaskScheduler::poll(std::size_t max_tasks) {
    if (is_shutdown()) {
        return 0;
    }

    std::vector<ReadyTask> tasks;
    collect_ready(tasks, max_tasks == static_cast<std::size_t>(-1)
                             ? std::numeric_limits<std::size_t>::max()
                             : max_tasks);

    std::size_t executed = 0;
    for (auto& ready : tasks) {
        const auto& record = ready.record;
        if (!record || record->canceled || ready.generation != record->generation) {
            continue;
        }

        if (!record->repeating) {
            if (record->validator && !record->validator()) {
                std::lock_guard<std::mutex> lock(mutex_);
                tasks_.erase(record->id);
                record->canceled = true;
                continue;
            }

            if (record->one_shot) {
                record->one_shot();
                ++executed;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.erase(record->id);
            record->canceled = true;
            continue;
        }

        auto context = record->repeat_context;
        if (record->repeat_validator && !record->repeat_validator(context)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (record->canceled) {
                continue;
            }
            ++record->repeat_context.validator_skip_count;
            ++record->generation;
            const auto delay = next_repeat_delay(*record);
            const auto next_due = Clock::now() + delay;
            record->repeat_context.current_interval = delay;
            record->repeat_context.last_due = next_due;
            delayed_.push(DelayedTask{next_due, record, record->generation});
            continue;
        }

        if (record->repeat) {
            const auto decision = record->repeat(context);
            ++executed;

            if (decision == RepeatDecision::kStop) {
                std::lock_guard<std::mutex> lock(mutex_);
                tasks_.erase(record->id);
                record->canceled = true;
                continue;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (record->canceled) {
            continue;
        }
        ++record->repeat_context.run_count;
        ++record->generation;
        const auto delay = next_repeat_delay(*record);
        const auto next_due = Clock::now() + delay;
        record->repeat_context.current_interval = delay;
        record->repeat_context.last_due = next_due;
        delayed_.push(DelayedTask{next_due, record, record->generation});
    }

    return executed;
}

void TaskScheduler::collect_ready(std::vector<ReadyTask>& out, std::size_t max_tasks) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();

    while (!delayed_.empty() && delayed_.top().due <= now) {
        auto delayed = delayed_.top();
        delayed_.pop();
        if (!delayed.record || delayed.record->canceled || delayed.generation != delayed.record->generation) {
            continue;
        }
        ready_.push(ReadyTask{delayed.record, delayed.generation});
    }

    while (!ready_.empty() && out.size() < max_tasks) {
        out.emplace_back(std::move(ready_.front()));
        ready_.pop();
    }
}

bool TaskScheduler::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_.empty() && delayed_.empty() && tasks_.empty();
}

} // namespace server::core::concurrent
