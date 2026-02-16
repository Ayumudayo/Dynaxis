#include "server/core/storage/db_worker_pool.hpp"

#include <stdexcept>
#include <utility>

#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"

/**
 * @brief DbWorkerPool의 워커 루프/커밋/롤백 처리 구현입니다.
 *
 * DB 예외를 작업 단위로 격리해 전체 워커 중단을 방지하고,
 * 큐 깊이/실패율 메트릭을 함께 기록해 병목 원인을 추적할 수 있게 합니다.
 */
namespace server::core::storage {

namespace {
std::size_t normalize_worker_count(std::size_t count) {
    if (count == 0) {
        count = std::thread::hardware_concurrency();
    }
    return count == 0 ? 1 : count;
}
} // namespace

DbWorkerPool::DbWorkerPool(std::shared_ptr<IConnectionPool> pool, std::size_t queue_capacity)
    : pool_(std::move(pool))
    , queue_(queue_capacity)
    , queue_capacity_(queue_capacity) {
    if (!pool_) {
        throw std::invalid_argument("DbWorkerPool requires a valid connection pool");
    }

    runtime_metrics::register_db_job_queue_capacity(queue_capacity_);
}

DbWorkerPool::~DbWorkerPool() {
    stop();
}

void DbWorkerPool::start(std::size_t worker_count) {
    if (running_.exchange(true)) {
        return;
    }
    stopping_.store(false);
    
    worker_count = normalize_worker_count(worker_count);
    workers_.reserve(worker_count);
    
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&DbWorkerPool::worker_loop, this, i);
    }
    
    log::info("DbWorkerPool started with " + std::to_string(worker_count) + " workers");
}

void DbWorkerPool::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    stopping_.store(true);
    queue_.stop(); // Wake up all waiting threads
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    log::info("DbWorkerPool stopped");
}

bool DbWorkerPool::running() const {
    return running_.load(std::memory_order_relaxed);
}

void DbWorkerPool::submit(Task task, bool auto_commit) {
    if (!task) {
        return;
    }
    if (!running_) {
        throw std::runtime_error("DbWorkerPool is not running");
    }
    const auto wait_begin = std::chrono::steady_clock::now();
    queue_.push(Job{std::move(task), auto_commit});
    const auto waited = std::chrono::steady_clock::now() - wait_begin;
    if (queue_capacity_ > 0) {
        runtime_metrics::record_db_job_queue_push_wait(waited);
    }
    runtime_metrics::record_db_job_queue_depth(queue_.size());
}

void DbWorkerPool::worker_loop(std::size_t index) {
    (void)index;
    while (true) {
        auto job = queue_.pop_blocking();
        if (!job.has_value()) {
            if (stopping_.load(std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        runtime_metrics::record_db_job_queue_depth(queue_.size());
        // DB 호출은 외부 시스템 상태에 따라 예외가 자주 발생하므로,
        // 작업 단위별로 metrics를 남기고 계속 루프를 돌린다.
        try {
            process_job(*job);
            runtime_metrics::record_db_job_processed();
        } catch (const std::exception& ex) {
            runtime_metrics::record_db_job_failed();
            log::error(std::string("DbWorkerPool job exception: ") + ex.what());
        } catch (...) {
            runtime_metrics::record_db_job_failed();
            log::error("DbWorkerPool job exception: unknown error");
        }
    }
}

void DbWorkerPool::process_job(const Job& job) {
    auto unit = pool_->make_unit_of_work();
    if (!unit) {
        throw std::runtime_error("Failed to create unit_of_work from connection pool");
    }

    const bool auto_commit = job.auto_commit;
    // unit_of_work는 commit/rollback을 명시적으로 호출해야 하므로
    // auto_commit 여부에 따라 커서를 정리한다.
    try {
        job.task(*unit);
        if (auto_commit) {
            unit->commit();
        }
    } catch (...) {
        try {
            unit->rollback();
        } catch (...) {
            // ignore rollback failure
        }
        throw;
    }

    if (!auto_commit) {
        try {
            unit->rollback();
        } catch (...) {
            // no-op: caller may already have committed
        }
    }
}

} // namespace server::core::storage
