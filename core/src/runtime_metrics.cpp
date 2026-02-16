#include "server/core/runtime_metrics.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace server::core::runtime_metrics {

namespace {
// 런타임에서 빠르게 꺼내 쓸 수 있도록 모든 카운터를 단일 구조체의 원자 타입으로 모아둔다.
struct RuntimeCounters {
    std::atomic<std::uint64_t> accept_total{0};

    std::atomic<std::uint64_t> session_started_total{0};
    std::atomic<std::uint64_t> session_stopped_total{0};
    std::atomic<std::uint64_t> session_active{0};
    std::atomic<std::uint64_t> session_timeout_total{0};
    std::atomic<std::uint64_t> session_write_timeout_total{0};
    std::atomic<std::uint64_t> heartbeat_timeout_total{0};
    std::atomic<std::uint64_t> send_queue_drop_total{0};
    std::atomic<std::uint64_t> packet_total{0};
    std::atomic<std::uint64_t> packet_error_total{0};
    std::atomic<std::uint64_t> packet_payload_sum_bytes{0};
    std::atomic<std::uint64_t> packet_payload_count{0};
    std::atomic<std::uint64_t> packet_payload_max_bytes{0};
    std::atomic<std::uint64_t> dispatch_total{0};
    std::atomic<std::uint64_t> dispatch_unknown_total{0};
    std::atomic<std::uint64_t> dispatch_exception_total{0};
    std::atomic<std::uint64_t> dispatch_latency_sum_ns{0};
    std::atomic<std::uint64_t> dispatch_latency_count{0};
    std::atomic<std::uint64_t> dispatch_latency_last_ns{0};
    std::atomic<std::uint64_t> dispatch_latency_max_ns{0};
    std::array<std::atomic<std::uint64_t>, kDispatchLatencyBucketUpperBoundsNs.size()> dispatch_latency_bucket_counts{};
    std::atomic<std::uint64_t> job_queue_depth{0};
    std::atomic<std::uint64_t> job_queue_depth_peak{0};
    std::atomic<std::uint64_t> job_queue_capacity{0};
    std::atomic<std::uint64_t> job_queue_reject_total{0};
    std::atomic<std::uint64_t> job_queue_push_wait_sum_ns{0};
    std::atomic<std::uint64_t> job_queue_push_wait_count{0};
    std::atomic<std::uint64_t> job_queue_push_wait_max_ns{0};
    std::atomic<std::uint64_t> memory_pool_capacity{0};
    std::atomic<std::uint64_t> memory_pool_in_use{0};
    std::atomic<std::uint64_t> memory_pool_in_use_peak{0};
    std::atomic<std::uint64_t> db_job_queue_depth{0};
    std::atomic<std::uint64_t> db_job_queue_depth_peak{0};
    std::atomic<std::uint64_t> db_job_queue_capacity{0};
    std::atomic<std::uint64_t> db_job_queue_reject_total{0};
    std::atomic<std::uint64_t> db_job_queue_push_wait_sum_ns{0};
    std::atomic<std::uint64_t> db_job_queue_push_wait_count{0};
    std::atomic<std::uint64_t> db_job_queue_push_wait_max_ns{0};
    std::atomic<std::uint64_t> db_job_processed_total{0};
    std::atomic<std::uint64_t> db_job_failed_total{0};
    // opcode 단위 집계를 위해 16bit 전체 공간을 미리 확보한다. (프로파일 기반으로 대부분은 0을 유지)
    std::array<std::atomic<std::uint64_t>, 65536> opcode_counters{};
};

RuntimeCounters& counters() {
    static RuntimeCounters c;
    return c;
}

} // namespace

void record_accept() {
    counters().accept_total.fetch_add(1, std::memory_order_relaxed);
}

void record_session_start() {
    counters().session_started_total.fetch_add(1, std::memory_order_relaxed);
    counters().session_active.fetch_add(1, std::memory_order_relaxed);
}

void record_session_stop() {
    counters().session_stopped_total.fetch_add(1, std::memory_order_relaxed);
    counters().session_active.fetch_sub(1, std::memory_order_relaxed);
}

void record_session_timeout() {
    counters().session_timeout_total.fetch_add(1, std::memory_order_relaxed);
}

void record_session_write_timeout() {
    counters().session_write_timeout_total.fetch_add(1, std::memory_order_relaxed);
}

void record_heartbeat_timeout() {
    counters().heartbeat_timeout_total.fetch_add(1, std::memory_order_relaxed);
}

void record_send_queue_drop() {
    counters().send_queue_drop_total.fetch_add(1, std::memory_order_relaxed);
}

void record_packet_ok() {
    counters().packet_total.fetch_add(1, std::memory_order_relaxed);
}

void record_packet_error() {
    counters().packet_error_total.fetch_add(1, std::memory_order_relaxed);
}

void record_packet_payload(std::size_t bytes) {
    counters().packet_payload_sum_bytes.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    counters().packet_payload_count.fetch_add(1, std::memory_order_relaxed);
    auto& max_ref = counters().packet_payload_max_bytes;
    std::uint64_t current = max_ref.load(std::memory_order_relaxed);
    const std::uint64_t value = static_cast<std::uint64_t>(bytes);
    // frame payload 최대치는 순서가 없으므로 CAS loop로 경합을 줄인다.
    while (current < value && !max_ref.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        // retry
    }
}

void record_dispatch_opcode(std::uint16_t opcode) {
    counters().opcode_counters[opcode].fetch_add(1, std::memory_order_relaxed);
}

void record_dispatch_attempt(bool handler_found, std::chrono::nanoseconds elapsed) {
    counters().dispatch_total.fetch_add(1, std::memory_order_relaxed);
    if (!handler_found) {
        counters().dispatch_unknown_total.fetch_add(1, std::memory_order_relaxed);
    }
    const auto ns = static_cast<std::uint64_t>(elapsed.count());
    counters().dispatch_latency_sum_ns.fetch_add(ns, std::memory_order_relaxed);
    counters().dispatch_latency_count.fetch_add(1, std::memory_order_relaxed);
    counters().dispatch_latency_last_ns.store(ns, std::memory_order_relaxed);

    auto& max_ref = counters().dispatch_latency_max_ns;
    std::uint64_t current_max = max_ref.load(std::memory_order_relaxed);
    // P99 등을 계산할 여지는 남겨두고, 일단 최대 지연만 추적해 장애 시점을 빠르게 찾는다.
    while (current_max < ns && !max_ref.compare_exchange_weak(current_max, ns, std::memory_order_relaxed)) {
        // retry until successfully updated or observed newer max
    }

    for (std::size_t i = 0; i < kDispatchLatencyBucketUpperBoundsNs.size(); ++i) {
        if (ns <= kDispatchLatencyBucketUpperBoundsNs[i]) {
            counters().dispatch_latency_bucket_counts[i].fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
}

void record_dispatch_exception() {
    counters().dispatch_exception_total.fetch_add(1, std::memory_order_relaxed);
}

void record_job_queue_depth(std::size_t depth) {
    counters().job_queue_depth.store(static_cast<std::uint64_t>(depth), std::memory_order_relaxed);
    auto& peak_ref = counters().job_queue_depth_peak;
    std::uint64_t current_peak = peak_ref.load(std::memory_order_relaxed);
    std::uint64_t value = static_cast<std::uint64_t>(depth);
    // worker tuning을 위해 최대 backlog만 저장하면 되므로 lock-free CAS로 최대값만 교체한다.
    while (current_peak < value && !peak_ref.compare_exchange_weak(current_peak, value, std::memory_order_relaxed)) {
        // retry
    }
}

void register_job_queue_capacity(std::size_t capacity) {
    counters().job_queue_capacity.store(static_cast<std::uint64_t>(capacity), std::memory_order_relaxed);
}

void record_job_queue_reject() {
    counters().job_queue_reject_total.fetch_add(1, std::memory_order_relaxed);
}

void record_job_queue_push_wait(std::chrono::nanoseconds waited) {
    const auto ns = static_cast<std::uint64_t>(waited.count() <= 0 ? 0 : waited.count());
    if (ns == 0) {
        return;
    }
    counters().job_queue_push_wait_sum_ns.fetch_add(ns, std::memory_order_relaxed);
    counters().job_queue_push_wait_count.fetch_add(1, std::memory_order_relaxed);

    auto& max_ref = counters().job_queue_push_wait_max_ns;
    std::uint64_t current_max = max_ref.load(std::memory_order_relaxed);
    while (current_max < ns && !max_ref.compare_exchange_weak(current_max, ns, std::memory_order_relaxed)) {
        // retry
    }
}

void record_db_job_queue_depth(std::size_t depth) {
    counters().db_job_queue_depth.store(static_cast<std::uint64_t>(depth), std::memory_order_relaxed);
    auto& peak_ref = counters().db_job_queue_depth_peak;
    std::uint64_t current_peak = peak_ref.load(std::memory_order_relaxed);
    std::uint64_t value = static_cast<std::uint64_t>(depth);
    // DB worker pool도 동일 로직으로 가장 심했던 backlog를 추적한다.
    while (current_peak < value && !peak_ref.compare_exchange_weak(current_peak, value, std::memory_order_relaxed)) {
        // retry
    }
}

void register_db_job_queue_capacity(std::size_t capacity) {
    counters().db_job_queue_capacity.store(static_cast<std::uint64_t>(capacity), std::memory_order_relaxed);
}

void record_db_job_queue_reject() {
    counters().db_job_queue_reject_total.fetch_add(1, std::memory_order_relaxed);
}

void record_db_job_queue_push_wait(std::chrono::nanoseconds waited) {
    const auto ns = static_cast<std::uint64_t>(waited.count() <= 0 ? 0 : waited.count());
    if (ns == 0) {
        return;
    }
    counters().db_job_queue_push_wait_sum_ns.fetch_add(ns, std::memory_order_relaxed);
    counters().db_job_queue_push_wait_count.fetch_add(1, std::memory_order_relaxed);

    auto& max_ref = counters().db_job_queue_push_wait_max_ns;
    std::uint64_t current_max = max_ref.load(std::memory_order_relaxed);
    while (current_max < ns && !max_ref.compare_exchange_weak(current_max, ns, std::memory_order_relaxed)) {
        // retry
    }
}

void record_db_job_processed() {
    counters().db_job_processed_total.fetch_add(1, std::memory_order_relaxed);
}

void record_db_job_failed() {
    counters().db_job_failed_total.fetch_add(1, std::memory_order_relaxed);
}

void register_memory_pool_capacity(std::size_t capacity) {
    const auto cap = static_cast<std::uint64_t>(capacity);
    counters().memory_pool_capacity.store(cap, std::memory_order_relaxed);

    auto& in_use_ref = counters().memory_pool_in_use;
    auto current_in_use = in_use_ref.load(std::memory_order_relaxed);
    if (current_in_use > cap) {
        in_use_ref.store(cap, std::memory_order_relaxed);
        current_in_use = cap;
    }

    auto& peak_ref = counters().memory_pool_in_use_peak;
    auto peak = peak_ref.load(std::memory_order_relaxed);
    if (peak > cap) {
        peak_ref.store(cap, std::memory_order_relaxed);
    }
}

void record_memory_pool_acquire() {
    auto current = counters().memory_pool_in_use.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& peak_ref = counters().memory_pool_in_use_peak;
    std::uint64_t peak = peak_ref.load(std::memory_order_relaxed);
    while (peak < current && !peak_ref.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
        // retry
    }
}

void record_memory_pool_release() {
    counters().memory_pool_in_use.fetch_sub(1, std::memory_order_relaxed);
}

Snapshot snapshot() {
    Snapshot snap{};
    auto& c = counters();
    snap.accept_total = c.accept_total.load(std::memory_order_relaxed);
    snap.session_started_total = c.session_started_total.load(std::memory_order_relaxed);
    snap.session_stopped_total = c.session_stopped_total.load(std::memory_order_relaxed);
    snap.session_active = c.session_active.load(std::memory_order_relaxed);
    snap.session_timeout_total = c.session_timeout_total.load(std::memory_order_relaxed);
    snap.session_write_timeout_total = c.session_write_timeout_total.load(std::memory_order_relaxed);
    snap.heartbeat_timeout_total = c.heartbeat_timeout_total.load(std::memory_order_relaxed);
    snap.send_queue_drop_total = c.send_queue_drop_total.load(std::memory_order_relaxed);
    snap.packet_total = c.packet_total.load(std::memory_order_relaxed);
    snap.packet_error_total = c.packet_error_total.load(std::memory_order_relaxed);
    snap.packet_payload_sum_bytes = c.packet_payload_sum_bytes.load(std::memory_order_relaxed);
    snap.packet_payload_count = c.packet_payload_count.load(std::memory_order_relaxed);
    snap.packet_payload_max_bytes = c.packet_payload_max_bytes.load(std::memory_order_relaxed);
    snap.dispatch_total = c.dispatch_total.load(std::memory_order_relaxed);
    snap.dispatch_unknown_total = c.dispatch_unknown_total.load(std::memory_order_relaxed);
    snap.dispatch_exception_total = c.dispatch_exception_total.load(std::memory_order_relaxed);
    snap.dispatch_latency_sum_ns = c.dispatch_latency_sum_ns.load(std::memory_order_relaxed);
    snap.dispatch_latency_count = c.dispatch_latency_count.load(std::memory_order_relaxed);
    snap.dispatch_latency_last_ns = c.dispatch_latency_last_ns.load(std::memory_order_relaxed);
    snap.dispatch_latency_max_ns = c.dispatch_latency_max_ns.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < snap.dispatch_latency_bucket_counts.size(); ++i) {
        snap.dispatch_latency_bucket_counts[i] = c.dispatch_latency_bucket_counts[i].load(std::memory_order_relaxed);
    }
    snap.job_queue_depth = c.job_queue_depth.load(std::memory_order_relaxed);
    snap.job_queue_depth_peak = c.job_queue_depth_peak.load(std::memory_order_relaxed);
    snap.job_queue_capacity = c.job_queue_capacity.load(std::memory_order_relaxed);
    snap.job_queue_reject_total = c.job_queue_reject_total.load(std::memory_order_relaxed);
    snap.job_queue_push_wait_sum_ns = c.job_queue_push_wait_sum_ns.load(std::memory_order_relaxed);
    snap.job_queue_push_wait_count = c.job_queue_push_wait_count.load(std::memory_order_relaxed);
    snap.job_queue_push_wait_max_ns = c.job_queue_push_wait_max_ns.load(std::memory_order_relaxed);
    snap.db_job_queue_depth = c.db_job_queue_depth.load(std::memory_order_relaxed);
    snap.db_job_queue_depth_peak = c.db_job_queue_depth_peak.load(std::memory_order_relaxed);
    snap.db_job_queue_capacity = c.db_job_queue_capacity.load(std::memory_order_relaxed);
    snap.db_job_queue_reject_total = c.db_job_queue_reject_total.load(std::memory_order_relaxed);
    snap.db_job_queue_push_wait_sum_ns = c.db_job_queue_push_wait_sum_ns.load(std::memory_order_relaxed);
    snap.db_job_queue_push_wait_count = c.db_job_queue_push_wait_count.load(std::memory_order_relaxed);
    snap.db_job_queue_push_wait_max_ns = c.db_job_queue_push_wait_max_ns.load(std::memory_order_relaxed);
    snap.db_job_processed_total = c.db_job_processed_total.load(std::memory_order_relaxed);
    snap.db_job_failed_total = c.db_job_failed_total.load(std::memory_order_relaxed);
    snap.memory_pool_capacity = c.memory_pool_capacity.load(std::memory_order_relaxed);
    snap.memory_pool_in_use = c.memory_pool_in_use.load(std::memory_order_relaxed);
    snap.memory_pool_in_use_peak = c.memory_pool_in_use_peak.load(std::memory_order_relaxed);

    for (std::size_t i = 0; i < c.opcode_counters.size(); ++i) {
        auto value = c.opcode_counters[i].load(std::memory_order_relaxed);
        if (value != 0) {
            snap.opcode_counts.emplace_back(static_cast<std::uint16_t>(i), value);
        }
    }

    return snap;
}

} // namespace server::core::runtime_metrics
