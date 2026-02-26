#pragma once
#include <cstddef>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace server::core {

using Job = std::function<void()>;

/**
 * @brief MPMC 작업 큐 (Thread-safe FIFO)
 *
 * 계약:
 * - `Push()`는 bounded 큐(`max_size > 0`)가 가득 찬 경우 공간이 생길 때까지 block 됩니다.
 * - `TryPush()`는 bounded 큐가 가득 찼거나 `Stop()` 이후면 즉시 false를 반환합니다.
 * - `Pop()`은 작업이 들어오거나 `Stop()` 호출 시까지 block 됩니다.
 * - `Stop()` 이후 `Pop()`은 큐 소진 후 빈 `Job`(nullptr)을 반환해 소비자 종료 신호로 사용됩니다.
 * - 빈 `Job` enqueue는 허용되지 않습니다.
 */
class JobQueue {
public:
    explicit JobQueue(std::size_t max_size = 0);

    /**
     * @brief 작업을 큐에 추가합니다.
     * @param job 실행할 함수 객체 (std::function<void()>)
     */
    void Push(Job job);

    // 큐가 가득 찼거나 stopping 상태면 false를 반환하고 job을 버립니다.
    bool TryPush(Job job);

    /**
     * @brief 작업을 큐에서 꺼냅니다. (Blocking)
     * 큐가 비어있으면 작업이 들어올 때까지 대기합니다.
     * @return 꺼낸 작업 (Stop() 호출 시 빈 작업 반환 가능)
     */
    Job Pop();

    /**
     * @brief 큐를 정지시키고 대기 중인 모든 스레드를 깨웁니다.
     */
    void Stop();

    std::size_t max_size() const;

private:
    std::queue<Job> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::size_t max_size_ = 0; // 0이면 무제한
};

} // namespace server::core
