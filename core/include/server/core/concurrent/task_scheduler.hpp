#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <unordered_map>
#include <vector>

namespace server::core::concurrent {

/**
 * @brief 즉시/지연/반복 작업을 단일 poll 루프에서 실행하는 경량 스케줄러입니다.
 *
 * 실행 스레드를 내부에서 소유하지 않으며, 호출자가 `poll()`을 주기적으로 호출해
 * 준비된 작업을 소비하는 pull 모델을 사용합니다.
 */
class TaskScheduler {
public:
    /** @brief 스케줄 타이밍 계산에 사용하는 단조 시계 타입입니다. */
    using Clock = std::chrono::steady_clock;
    /** @brief 실행할 작업 콜러블 타입입니다. */
    using Task = std::function<void()>;
    /** @brief 스케줄러 내부 작업/취소 식별자 타입입니다. */
    using TaskId = std::uint64_t;

    /** @brief 개별 예약 작업을 취소/재스케줄할 때 사용하는 토큰입니다. */
    struct CancelToken {
        TaskId value{0};

        explicit operator bool() const noexcept { return value != 0; }
        friend bool operator==(const CancelToken&, const CancelToken&) = default;
    };

    /** @brief 관련 작업 묶음을 한 번에 취소하기 위한 그룹 토큰입니다. */
    struct CancelGroup {
        TaskId value{0};

        explicit operator bool() const noexcept { return value != 0; }
        friend bool operator==(const CancelGroup&, const CancelGroup&) = default;
    };

    /** @brief 외부가 스케줄된 작업을 추적할 때 사용하는 핸들입니다. */
    struct ScheduleHandle {
        TaskId task_id{0};
        CancelToken cancel_token{};
        CancelGroup cancel_group{};

        explicit operator bool() const noexcept { return static_cast<bool>(cancel_token); }
    };

    /** @brief one-shot 작업 실행 직전 gate를 여는 선택적 validator입니다. */
    using Validator = std::function<bool()>;

    /** @brief one-shot 작업 예약 시 적용할 제어 옵션입니다. */
    struct TaskOptions {
        CancelGroup cancel_group{};
        Validator validator{};
    };

    /** @brief 반복 작업이 현재까지 몇 번 실행/skip 되었는지 알려 주는 실행 문맥입니다. */
    struct RepeatContext {
        std::size_t run_count{0};
        std::size_t validator_skip_count{0};
        Clock::time_point first_due{};
        Clock::time_point last_due{};
        Clock::duration current_interval{Clock::duration::zero()};
    };

    /** @brief 반복 작업이 다음 회차를 계속할지 멈출지 결정합니다. */
    enum class RepeatDecision : std::uint8_t {
        kContinue = 0,
        kStop = 1,
    };

    /** @brief 반복 작업 실행 콜백입니다. */
    using RepeatTask = std::function<RepeatDecision(const RepeatContext&)>;
    /** @brief 반복 작업 실행 직전 gate를 여는 validator입니다. */
    using RepeatValidator = std::function<bool(const RepeatContext&)>;

    /** @brief 반복 작업 주기 정책입니다. */
    struct RepeatPolicy {
        Clock::duration interval{Clock::duration::zero()};
        Clock::duration max_interval{Clock::duration::zero()};
        double backoff_multiplier{1.0};
        Clock::duration jitter{Clock::duration::zero()};
    };

    /** @brief 빈 스케줄러를 생성합니다. */
    TaskScheduler();
    /** @brief 스케줄러를 파괴하며 보류 작업을 정리합니다. */
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    /**
     * @brief 스케줄러를 종료하고 대기 중인 작업을 정리한다.
     *
     * 왜 필요한가?
     * - 서비스 종료 시 지연 큐에 남은 작업(heartbeat, health check 등)을 더 이상 실행하지 않게 해
     *   "종료 중인데 다시 작업이 살아나는" 레이스를 막는다.
     */
    void shutdown();

    /**
     * @brief 관련 작업들을 묶어 취소하기 위한 새 cancel group을 생성합니다.
     * @return 이후 예약 작업에 연결할 새 cancel group 토큰
     */
    [[nodiscard]] CancelGroup create_cancel_group();

    /**
     * @brief 작업을 즉시 실행 대기열에 넣는다(지연 없음).
     * @param task 즉시 실행 대기열에 추가할 작업
     *
     * 실제 실행은 poll() 호출 시점에 이뤄진다.
     * 즉, 이 클래스는 "실행 스레드"를 소유하지 않고 호출자 루프에 실행 책임을 둔다.
     */
    void post(Task task);

    /**
     * @brief 기본 제어 옵션으로 즉시 작업을 등록합니다.
     * @param task 즉시 실행 대기열에 추가할 작업
     * @return 생성된 작업 핸들
     */
    [[nodiscard]] ScheduleHandle post_controlled(Task task);
    /**
     * @brief cancel token/group/validator가 포함된 즉시 작업을 등록합니다.
     * @param task 즉시 실행 대기열에 추가할 작업
     * @param options cancel group/validator를 담은 제어 옵션
     * @return 생성된 작업 핸들
     */
    [[nodiscard]] ScheduleHandle post_controlled(Task task, TaskOptions options);

    /**
     * @brief delay 이후 실행되도록 작업을 예약한다.
     * @param task 실행할 작업
     * @param delay 지연 시간
     *
     * 내부적으로 due time 기준 우선순위 큐를 사용하며,
     * 별도 타이머 스레드 없이 poll()에서 만료 작업을 수거한다.
     */
    void schedule(Task task, Clock::duration delay);

    /**
     * @brief 기본 제어 옵션으로 지연 작업을 등록합니다.
     * @param task 실행할 작업
     * @param delay 현재 시점부터의 지연 시간
     * @return 생성된 작업 핸들
     */
    [[nodiscard]] ScheduleHandle schedule_controlled(Task task, Clock::duration delay);
    /**
     * @brief cancel token/group/validator가 포함된 지연 작업을 등록합니다.
     * @param task 실행할 작업
     * @param delay 현재 시점부터의 지연 시간
     * @param options cancel group/validator를 담은 제어 옵션
     * @return 생성된 작업 핸들
     */
    [[nodiscard]] ScheduleHandle schedule_controlled(Task task,
                                                     Clock::duration delay,
                                                     TaskOptions options);

    /**
     * @brief interval 간격으로 반복 실행되는 작업을 예약한다.
     * @param task 반복 실행할 작업
     * @param interval 반복 주기
     *
     * 구현은 "자기 재스케줄링" 방식이다.
     * 한 번 실행된 작업이 다음 실행을 다시 등록하기 때문에,
     * shutdown 이후에는 재등록이 즉시 차단되어 깔끔하게 정지된다.
     */
    void schedule_every(Task task, Clock::duration interval);

    /**
     * @brief repeat context와 repeat policy를 가진 반복 작업을 등록합니다.
     * @param task 반복 실행 콜백
     * @param policy interval/jitter/backoff 정책
     * @param validator 실행 직전 gate validator
     * @param cancel_group 연관 작업 묶음 취소용 group
     * @return 생성된 작업 핸들
     */
    [[nodiscard]] ScheduleHandle schedule_every_controlled(RepeatTask task, RepeatPolicy policy);
    [[nodiscard]] ScheduleHandle schedule_every_controlled(RepeatTask task,
                                                           RepeatPolicy policy,
                                                           RepeatValidator validator);
    [[nodiscard]] ScheduleHandle schedule_every_controlled(RepeatTask task,
                                                           RepeatPolicy policy,
                                                           RepeatValidator validator,
                                                           CancelGroup cancel_group);

    /**
     * @brief cancel token 하나를 취소합니다.
     * @param token 취소할 개별 작업 토큰
     * @return 취소 대상으로 표시된 live 작업이 있으면 `true`, 이미 끝났거나 알 수 없으면 `false`
     */
    bool cancel(CancelToken token);
    /**
     * @brief 같은 cancel group에 속한 작업을 모두 취소합니다.
     * @param group 일괄 취소할 작업 묶음 토큰
     * @return 이번 호출에서 취소 대상으로 표시된 작업 수
     */
    std::size_t cancel(CancelGroup group);
    /**
     * @brief 아직 실행 전인 작업의 다음 due 시각을 다시 잡습니다.
     * @param token 재스케줄할 개별 작업 토큰
     * @param delay 현재 시점부터 다시 계산할 새 지연 시간
     * @return due 시각을 새 generation으로 갱신했으면 `true`, 이미 끝났거나 알 수 없으면 `false`
     */
    bool reschedule(CancelToken token, Clock::duration delay);
    /**
     * @brief 반복 작업의 interval/jitter/backoff 정책을 갱신합니다.
     * @param token 갱신할 반복 작업 토큰
     * @param policy 새 반복 정책
     * @return 반복 작업의 정책을 갱신했으면 `true`, 해당 토큰이 반복 작업이 아니거나 이미 끝났으면 `false`
     */
    bool update_repeat_policy(CancelToken token, RepeatPolicy policy);

    /**
     * @brief 현재 실행 가능한 작업을 최대 max_tasks개까지 실행한다.
     *
     * 메인 루프에서 주기적으로 호출하는 pull 모델이므로,
     * 처리량 상한(max_tasks)을 조절해 한 틱에서 작업이 너무 오래 점유하지 않게 할 수 있다.
     *
     * @param max_tasks 한 번에 처리할 최대 작업 수
     * @return 실제 실행된 작업 수
     */
    std::size_t poll(std::size_t max_tasks = static_cast<std::size_t>(-1));

    /**
     * @brief 즉시/지연 큐가 모두 비어 있는지 확인합니다.
     * @return 실행 대기 작업이 없으면 `true`
     */
    bool empty() const;

private:
    struct TaskRecord;

    /** @brief 지연 실행 작업 엔트리입니다. */
    struct DelayedTask {
        Clock::time_point due;
        std::shared_ptr<TaskRecord> record;
        std::uint64_t generation{0};
    };

    /** @brief due 시각이 빠른 작업이 우선되도록 하는 비교자입니다. */
    struct CompareDue {
        bool operator()(const DelayedTask& a, const DelayedTask& b) const {
            return a.due > b.due;
        }
    };

    /** @brief ready queue로 승격된 작업과 generation을 묶은 내부 엔트리입니다. */
    struct ReadyTask {
        std::shared_ptr<TaskRecord> record;
        std::uint64_t generation{0};
    };

    void collect_ready(std::vector<ReadyTask>& out, std::size_t max_tasks);
    bool is_shutdown() const;
    [[nodiscard]] ScheduleHandle make_handle(TaskId id, CancelGroup group) const noexcept;
    [[nodiscard]] Clock::duration apply_jitter(Clock::duration delay, Clock::duration jitter);
    [[nodiscard]] Clock::duration next_repeat_delay(const TaskRecord& record);

    mutable std::mutex mutex_;
    std::queue<ReadyTask> ready_;
    std::priority_queue<DelayedTask, std::vector<DelayedTask>, CompareDue> delayed_;
    std::unordered_map<TaskId, std::shared_ptr<TaskRecord>> tasks_;
    std::mt19937_64 jitter_rng_{std::random_device{}()};
    std::atomic_bool shutdown_{false};
    std::atomic<TaskId> next_task_id_{1};
    std::atomic<TaskId> next_group_id_{1};
};

} // namespace server::core::concurrent

