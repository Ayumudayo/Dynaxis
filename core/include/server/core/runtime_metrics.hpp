#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>

namespace server::core::runtime_metrics {

/** @brief dispatch `processing_place` 분류 개수입니다. 배열 크기와 라벨 수를 같은 상수로 묶어 드리프트를 막습니다. */
inline constexpr std::size_t kDispatchProcessingPlaceCount = 3;
/** @brief RUDP fallback reason 라벨 개수입니다. 이유 enum과 메트릭 직렬화 테이블이 같은 크기를 보게 합니다. */
inline constexpr std::size_t kRudpFallbackReasonCount = 6;

/** @brief RUDP RTT 히스토그램 버킷 상한(ms)입니다. 지나치게 촘촘하면 비용이 늘고, 너무 성기면 tail 해석력이 떨어지므로 운영 구간만 남깁니다. */
inline constexpr std::array<std::uint64_t, 11> kRudpRttBucketUpperBoundsMs = {
    1,
    2,
    5,
    10,
    20,
    50,
    100,
    200,
    500,
    1000,
    2000,
};

/** @brief RUDP가 기본 경로를 포기한 이유 분류입니다. 단순 실패 총합보다 "왜 포기했는가"를 남겨야 튜닝 방향이 보입니다. */
enum class RudpFallbackReason : std::uint8_t {
    kHandshakeTimeout = 0,
    kIdleTimeout,
    kProtocolError,
    kInflightLimit,
    kDisabled,
    kOther,
};

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
 * @brief 런타임 누적 카운터를 읽기 전용 스냅샷으로 캡처한 구조체입니다.
 *
 * 메트릭 노출 시 원자 카운터를 하나씩 직접 직렬화하면, 렌더링 코드가 내부 저장 형식에 과도하게 결합됩니다.
 * 스냅샷 값 객체를 한 번 만든 뒤 직렬화 계층으로 넘기면 락/원자 접근 시간을 줄이면서도
 * "어떤 시점의 값 묶음"을 비교적 일관되게 다룰 수 있습니다.
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
    std::array<std::uint64_t, kDispatchProcessingPlaceCount> dispatch_processing_place_calls_total{};
    std::array<std::uint64_t, kDispatchProcessingPlaceCount> dispatch_processing_place_reject_total{};
    std::array<std::uint64_t, kDispatchProcessingPlaceCount> dispatch_processing_place_exception_total{};
    std::uint64_t exception_recoverable_total{0};
    std::uint64_t exception_fatal_total{0};
    std::uint64_t exception_ignored_total{0};
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
    std::uint64_t log_async_queue_depth{0};
    std::uint64_t log_async_queue_capacity{0};
    std::uint64_t log_async_queue_drop_total{0};
    std::uint64_t log_async_flush_total{0};
    std::uint64_t log_async_flush_latency_sum_ns{0};
    std::uint64_t log_async_flush_latency_max_ns{0};
    std::uint64_t log_masked_fields_total{0};
    std::uint64_t http_active_connections{0};
    std::uint64_t http_connection_limit_reject_total{0};
    std::uint64_t http_auth_reject_total{0};
    std::uint64_t http_header_timeout_total{0};
    std::uint64_t http_body_timeout_total{0};
    std::uint64_t http_header_oversize_total{0};
    std::uint64_t http_body_oversize_total{0};
    std::uint64_t http_bad_request_total{0};
    std::uint64_t runtime_setting_reload_attempt_total{0};
    std::uint64_t runtime_setting_reload_success_total{0};
    std::uint64_t runtime_setting_reload_failure_total{0};
    std::uint64_t runtime_setting_reload_latency_sum_ns{0};
    std::uint64_t runtime_setting_reload_latency_max_ns{0};
    std::uint64_t rudp_handshake_ok_total{0};
    std::uint64_t rudp_handshake_fail_total{0};
    std::uint64_t rudp_retransmit_total{0};
    std::uint64_t rudp_inflight_packets{0};
    std::uint64_t rudp_rtt_ms_sum{0};
    std::uint64_t rudp_rtt_ms_count{0};
    std::uint64_t rudp_rtt_ms_max{0};
    std::array<std::uint64_t, kRudpRttBucketUpperBoundsMs.size()> rudp_rtt_ms_bucket_counts{};
    std::array<std::uint64_t, kRudpFallbackReasonCount> rudp_fallback_total{};
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

/** @brief processing_place별 디스패치 시도 건수를 기록합니다. */
void record_dispatch_processing_place_call(std::size_t place_index);
/** @brief processing_place별 디스패치 거절 건수를 기록합니다. */
void record_dispatch_processing_place_reject(std::size_t place_index);
/** @brief processing_place별 디스패치 예외 건수를 기록합니다. */
void record_dispatch_processing_place_exception(std::size_t place_index);
/** @brief 복구 가능한 경계 예외를 기록합니다. */
void record_exception_recoverable();
/** @brief 치명 경계 예외를 기록합니다. */
void record_exception_fatal();
/** @brief 무시 가능한 경계 예외를 기록합니다. */
void record_exception_ignored();
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

/** @brief 비동기 로그 큐 현재 깊이를 기록합니다. */
void record_log_async_queue_depth(std::size_t depth);
/** @brief 비동기 로그 큐 용량을 등록합니다. */
void register_log_async_queue_capacity(std::size_t capacity);
/** @brief 비동기 로그 큐 드롭 건수를 기록합니다. */
void record_log_async_queue_drop();
/** @brief 비동기 로그 flush 지연을 기록합니다. */
void record_log_async_flush_latency(std::chrono::nanoseconds elapsed);
/** @brief 로그 마스킹 필드 증가분을 기록합니다. */
void record_log_masked_fields(std::uint64_t count = 1);

/** @brief HTTP active connection 현재값을 기록합니다. */
void set_http_active_connections(std::size_t active);
/** @brief HTTP conn-limit 거절 건수를 기록합니다. */
void record_http_connection_limit_reject();
/** @brief HTTP auth 거절 건수를 기록합니다. */
void record_http_auth_reject();
/** @brief HTTP header read timeout 건수를 기록합니다. */
void record_http_header_timeout();
/** @brief HTTP body read timeout 건수를 기록합니다. */
void record_http_body_timeout();
/** @brief HTTP header oversize 거절 건수를 기록합니다. */
void record_http_header_oversize();
/** @brief HTTP body oversize 거절 건수를 기록합니다. */
void record_http_body_oversize();
/** @brief HTTP bad request 거절 건수를 기록합니다. */
void record_http_bad_request();

/** @brief 런타임 설정 리로드 시도 건수를 기록합니다. */
void record_runtime_setting_reload_attempt();
/** @brief 런타임 설정 리로드 성공 건수를 기록합니다. */
void record_runtime_setting_reload_success();
/** @brief 런타임 설정 리로드 실패 건수를 기록합니다. */
void record_runtime_setting_reload_failure();
/** @brief 런타임 설정 리로드 지연을 기록합니다. */
void record_runtime_setting_reload_latency(std::chrono::nanoseconds elapsed);

/** @brief RUDP handshake 결과를 기록합니다. */
void record_rudp_handshake_result(bool ok);
/** @brief RUDP 재전송 건수를 누적합니다. */
void record_rudp_retransmit(std::uint64_t count = 1);
/** @brief 현재 RUDP inflight 패킷 수를 기록합니다. */
void set_rudp_inflight_packets(std::size_t packets);
/** @brief RUDP RTT 샘플(ms)을 기록합니다. */
void record_rudp_rtt_ms(std::uint32_t rtt_ms);
/** @brief RUDP fallback 원인을 기록합니다. */
void record_rudp_fallback(RudpFallbackReason reason);

/**
 * @brief 현재 런타임 카운터 스냅샷을 반환합니다.
 * @return 시점 기준 메트릭 스냅샷
 */
Snapshot snapshot();

} // namespace server::core::runtime_metrics
