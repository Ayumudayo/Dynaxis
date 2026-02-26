#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
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
     * @brief 작업을 즉시 실행 대기열에 넣는다(지연 없음).
     * @param task 즉시 실행 대기열에 추가할 작업
     *
     * 실제 실행은 poll() 호출 시점에 이뤄진다.
     * 즉, 이 클래스는 "실행 스레드"를 소유하지 않고 호출자 루프에 실행 책임을 둔다.
     */
    void post(Task task);

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
    /** @brief 지연 실행 작업 엔트리입니다. */
    struct DelayedTask {
        Clock::time_point due;
        Task task;
    };

    /** @brief due 시각이 빠른 작업이 우선되도록 하는 비교자입니다. */
    struct CompareDue {
        bool operator()(const DelayedTask& a, const DelayedTask& b) const {
            return a.due > b.due;
        }
    };

    void collect_ready(std::vector<Task>& out, std::size_t max_tasks);
    bool is_shutdown() const;

    mutable std::mutex mutex_;
    std::queue<Task> ready_;
    std::priority_queue<DelayedTask, std::vector<DelayedTask>, CompareDue> delayed_;
    std::atomic_bool shutdown_{false};
};

} // namespace server::core::concurrent

