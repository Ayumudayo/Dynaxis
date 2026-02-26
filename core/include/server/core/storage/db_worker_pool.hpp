#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "server/core/concurrent/locked_queue.hpp"
#include "server/core/storage/connection_pool.hpp"

namespace server::core::storage {

/**
 * @brief DB 작업을 백그라운드 워커 스레드에서 처리하는 풀입니다.
 *
 * API 요청 경로에서 DB I/O를 직접 수행하지 않고 큐로 넘겨,
 * 응답 지연 급증과 이벤트 루프 블로킹을 줄이기 위한 구성입니다.
 */
class DbWorkerPool {
public:
    using Task = std::function<void(IUnitOfWork&)>;

    /** @brief 워커가 처리할 단일 DB 작업 단위입니다. */
    struct Job {
        Task task;
        bool auto_commit{true};
    };

    /**
     * @brief DB 워커 풀을 생성합니다.
     * @param pool UnitOfWork를 제공할 DB 연결 풀
     * @param queue_capacity 대기 큐 최대 길이
     */
    explicit DbWorkerPool(std::shared_ptr<IConnectionPool> pool, std::size_t queue_capacity = 4096);
    ~DbWorkerPool();

    DbWorkerPool(const DbWorkerPool&) = delete;
    DbWorkerPool& operator=(const DbWorkerPool&) = delete;

    /**
     * @brief 워커 스레드를 시작합니다.
     * @param worker_count 시작할 워커 스레드 수
     */
    void start(std::size_t worker_count);

    /** @brief 워커 스레드를 중지하고 잔여 작업을 정리합니다. */
    void stop();

    /**
     * @brief 실행 중 여부를 반환합니다.
     * @return 워커 풀이 실행 중이면 `true`
     */
    bool running() const;

    /**
     * @brief DB 작업을 큐에 제출합니다.
     * @param task UnitOfWork를 받아 수행할 작업
     * @param auto_commit true면 작업 후 commit을 자동 수행
     */
    void submit(Task task, bool auto_commit = true);

private:
    void worker_loop(std::size_t index);
    void process_job(const Job& job);

    std::shared_ptr<IConnectionPool> pool_;
    concurrent::LockedWaitQueue<Job> queue_;
    std::size_t queue_capacity_{0};
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
};

} // namespace server::core::storage
