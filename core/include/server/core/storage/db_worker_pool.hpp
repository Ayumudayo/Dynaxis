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

class DbWorkerPool {
public:
    using Task = std::function<void(IUnitOfWork&)>;

    struct Job {
        Task task;
        bool auto_commit{true};
    };

    explicit DbWorkerPool(std::shared_ptr<IConnectionPool> pool, std::size_t queue_capacity = 4096);
    ~DbWorkerPool();

    DbWorkerPool(const DbWorkerPool&) = delete;
    DbWorkerPool& operator=(const DbWorkerPool&) = delete;

    void start(std::size_t worker_count);
    void stop();
    bool running() const;

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
