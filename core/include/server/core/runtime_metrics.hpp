#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>

namespace server::core::runtime_metrics {

/**
 * @brief 디스패치 지연시간 히스토그램 버킷 상한(ns) 목록입니다.
 *
 * Prometheus histogram bucket으로 직렬화할 때 사용하며,
 * 0.1ms부터 5s까지 운영에서 자주 관찰되는 구간을 커버합니다.
 */
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

/**
 * @brief 런타임 누적 카운터를 스냅샷으로 캡처한 구조체입니다.
 *
 * 메트릭 노출 시 락 점유 시간을 줄이기 위해,
 * 내부 원자 카운터를 이 구조체로 복사해 직렬화 계층으로 전달합니다.
 */
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

/** @brief 신규 accept 이벤트를 1건 기록합니다. */
void record_accept();
/** @brief 세션 시작 이벤트를 1건 기록합니다. */
void record_session_start();
/** @brief 세션 종료 이벤트를 1건 기록합니다. */
void record_session_stop();
/** @brief 패킷 정상 처리 건수를 기록합니다. */
void record_packet_ok();
/** @brief 패킷 오류 건수를 기록합니다. */
void record_packet_error();

/**
 * @brief 디스패치 시도와 소요 시간을 기록합니다.
 * @param handler_found 핸들러를 찾았으면 true
 * @param elapsed 디스패치 처리 소요 시간
 */
void record_dispatch_attempt(bool handler_found, std::chrono::nanoseconds elapsed);

/** @brief 디스패치 중 예외 발생 건수를 기록합니다. */
void record_dispatch_exception();
/** @brief 읽기 타임아웃 종료 건수를 기록합니다. */
void record_session_timeout();
/** @brief 쓰기 타임아웃 종료 건수를 기록합니다. */
void record_session_write_timeout();
/** @brief heartbeat 타임아웃 종료 건수를 기록합니다. */
void record_heartbeat_timeout();
/** @brief 송신 큐 초과로 인한 드롭 건수를 기록합니다. */
void record_send_queue_drop();

/**
 * @brief 패킷 payload 크기 통계를 기록합니다.
 * @param bytes payload 바이트 수
 */
void record_packet_payload(std::size_t bytes);

/**
 * @brief opcode별 디스패치 횟수를 기록합니다.
 * @param opcode 처리한 opcode
 */
void record_dispatch_opcode(std::uint16_t opcode);

/**
 * @brief 메인 JobQueue 현재 깊이를 기록합니다.
 * @param depth 현재 큐 깊이
 */
void record_job_queue_depth(std::size_t depth);
/**
 * @brief 메인 JobQueue 용량을 등록합니다.
 * @param capacity 큐 최대 용량
 */
void register_job_queue_capacity(std::size_t capacity);
/** @brief 메인 JobQueue 제출 거부 횟수를 기록합니다. */
void record_job_queue_reject();

/**
 * @brief 메인 JobQueue push 대기 시간을 기록합니다.
 * @param waited push가 블록된 시간
 */
void record_job_queue_push_wait(std::chrono::nanoseconds waited);

/**
 * @brief DB JobQueue 현재 깊이를 기록합니다.
 * @param depth 현재 DB 큐 깊이
 */
void record_db_job_queue_depth(std::size_t depth);
/**
 * @brief DB JobQueue 용량을 등록합니다.
 * @param capacity DB 큐 최대 용량
 */
void register_db_job_queue_capacity(std::size_t capacity);
/** @brief DB JobQueue 제출 거부 횟수를 기록합니다. */
void record_db_job_queue_reject();

/**
 * @brief DB JobQueue push 대기 시간을 기록합니다.
 * @param waited push가 블록된 시간
 */
void record_db_job_queue_push_wait(std::chrono::nanoseconds waited);

/** @brief DB 작업 처리 성공 건수를 기록합니다. */
void record_db_job_processed();
/** @brief DB 작업 처리 실패 건수를 기록합니다. */
void record_db_job_failed();

/**
 * @brief 메모리 풀 총 capacity를 등록합니다.
 * @param capacity 풀 전체 블록 용량
 */
void register_memory_pool_capacity(std::size_t capacity);
/** @brief 메모리 풀 사용량 증가를 기록합니다. */
void record_memory_pool_acquire();
/** @brief 메모리 풀 사용량 감소를 기록합니다. */
void record_memory_pool_release();

/**
 * @brief 현재 런타임 카운터 스냅샷을 반환합니다.
 * @return 시점 기준 메트릭 스냅샷
 */
Snapshot snapshot();

} // namespace server::core::runtime_metrics
