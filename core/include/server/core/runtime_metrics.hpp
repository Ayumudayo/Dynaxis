#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>

namespace server::core::runtime_metrics {

inline constexpr std::array<std::uint64_t, 15> kDispatchLatencyBucketUpperBoundsNs = {
    100'000,        // 0.1ms
    250'000,        // 0.25ms
    500'000,        // 0.5ms
    1'000'000,      // 1ms
    2'500'000,      // 2.5ms
    5'000'000,      // 5ms
    10'000'000,     // 10ms
    25'000'000,     // 25ms
    50'000'000,     // 50ms
    100'000'000,    // 100ms
    250'000'000,    // 250ms
    500'000'000,    // 500ms
    1'000'000'000,  // 1s
    2'500'000'000,  // 2.5s
    5'000'000'000   // 5s
};

struct Snapshot {
    std::uint64_t accept_total{0};
    std::uint64_t session_started_total{0};
    std::uint64_t session_stopped_total{0};
    std::uint64_t session_active{0};
    std::uint64_t session_timeout_total{0};
    std::uint64_t session_write_timeout_total{0};
    std::uint64_t heartbeat_timeout_total{0};
    std::uint64_t send_queue_drop_total{0};
    std::uint64_t packet_total{0};
    std::uint64_t packet_error_total{0};
    std::uint64_t packet_payload_sum_bytes{0};
    std::uint64_t packet_payload_count{0};
    std::uint64_t packet_payload_max_bytes{0};
    std::uint64_t dispatch_total{0};
    std::uint64_t dispatch_unknown_total{0};
    std::uint64_t dispatch_exception_total{0};
    std::uint64_t dispatch_latency_sum_ns{0};
    std::uint64_t dispatch_latency_count{0};
    std::uint64_t dispatch_latency_last_ns{0};
    std::uint64_t dispatch_latency_max_ns{0};
    std::array<std::uint64_t, kDispatchLatencyBucketUpperBoundsNs.size()> dispatch_latency_bucket_counts{};
    std::uint64_t job_queue_depth{0};
    std::uint64_t job_queue_depth_peak{0};
    std::uint64_t job_queue_capacity{0};
    std::uint64_t job_queue_reject_total{0};
    std::uint64_t job_queue_push_wait_sum_ns{0};
    std::uint64_t job_queue_push_wait_count{0};
    std::uint64_t job_queue_push_wait_max_ns{0};
    std::uint64_t db_job_queue_depth{0};
    std::uint64_t db_job_queue_depth_peak{0};
    std::uint64_t db_job_queue_capacity{0};
    std::uint64_t db_job_queue_reject_total{0};
    std::uint64_t db_job_queue_push_wait_sum_ns{0};
    std::uint64_t db_job_queue_push_wait_count{0};
    std::uint64_t db_job_queue_push_wait_max_ns{0};
    std::uint64_t db_job_processed_total{0};
    std::uint64_t db_job_failed_total{0};
    std::uint64_t memory_pool_capacity{0};
    std::uint64_t memory_pool_in_use{0};
    std::uint64_t memory_pool_in_use_peak{0};
    std::vector<std::pair<std::uint16_t, std::uint64_t>> opcode_counts;
};

void record_accept();
void record_session_start();
void record_session_stop();
void record_packet_ok();
void record_packet_error();
void record_dispatch_attempt(bool handler_found, std::chrono::nanoseconds elapsed);
void record_dispatch_exception();
void record_session_timeout();
void record_session_write_timeout();
void record_heartbeat_timeout();
void record_send_queue_drop();
void record_packet_payload(std::size_t bytes);
void record_dispatch_opcode(std::uint16_t opcode);
void record_job_queue_depth(std::size_t depth);
void register_job_queue_capacity(std::size_t capacity);
void record_job_queue_reject();
void record_job_queue_push_wait(std::chrono::nanoseconds waited);
void record_db_job_queue_depth(std::size_t depth);
void register_db_job_queue_capacity(std::size_t capacity);
void record_db_job_queue_reject();
void record_db_job_queue_push_wait(std::chrono::nanoseconds waited);
void record_db_job_processed();
void record_db_job_failed();
void register_memory_pool_capacity(std::size_t capacity);
void record_memory_pool_acquire();
void record_memory_pool_release();
Snapshot snapshot();

} // namespace server::core::runtime_metrics
