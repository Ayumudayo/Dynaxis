#pragma once
#include <vector>
#include <thread>
#include <atomic>

namespace server::core {

class JobQueue;

/** @brief JobQueue 기반 워커 스레드 풀 수명주기를 관리합니다. */
class ThreadManager {
public:
    /**
     * @brief ThreadManager 생성자
     * @param job_queue 워커 스레드들이 작업을 가져올 큐
     */
    ThreadManager(JobQueue& job_queue);
    ~ThreadManager();

    /**
     * @brief 지정된 수만큼 워커 스레드를 생성하고 시작합니다.
     * @param num_threads 생성할 스레드 수
     *
     * 계약:
     * - `num_threads <= 0`이면 no-op 입니다.
     * - 이미 시작된 상태에서 재호출하면 no-op 입니다.
     * - 큐에 들어가는 작업은 예외를 밖으로 던지지 않는 것을 권장합니다.
     */
    void Start(int num_threads);

    /**
     * @brief 모든 워커 스레드를 정지하고 종료될 때까지 대기합니다.
     *
     * 계약:
     * - 여러 번 호출해도 안전합니다(idempotent).
     */
    void Stop();

private:
    void WorkerLoop();

    JobQueue& job_queue_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopped_{true};
};

} // namespace server::core
