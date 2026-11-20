#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dynaxis::admin {

/**
 * @brief Redis RESP ping 및 Write-Behind DLQ 길이 조회 결과입니다.
 */
struct RedisProbeResult {
    bool healthy{false};
    long long dlq_length{-1};
    std::uint32_t latency_ms{0};
    std::string error;
};

/**
 * @brief Write-Behind DLQ 스트림(`session_events_dlq`)에 격리된 단일 이벤트 미리보기 항목입니다.
 */
struct WbDlqPreviewEntry {
    std::string stream_id;
    std::string orig_event_id;
    std::string type;
    std::string ts_ms;
    std::string user_id;
    std::string session_id;
    std::string room_id;
    int retry_count{0};
    std::string error;
};

/**
 * @brief Write-Behind 기본/DLQ/Dead 스트림 길이 및 DLQ 항목 미리보기 스냅샷입니다.
 */
struct WbStreamsSnapshot {
    bool ok{false};
    long long pending_stream_length{-1};
    long long dlq_length{-1};
    long long dead_length{-1};
    std::vector<WbDlqPreviewEntry> preview_entries;
    std::string error;
};

/**
 * @brief Redis RESP 엔드포인트에 제한 시간 내에 `PING` 및 DLQ `XLEN` 조회를 수행합니다.
 * @param redis_uri `tcp://[:password@]host:port` 또는 `redis://...` URI (비어 있으면 `host`/`port` 직접 사용)
 * @param fallback_host `redis_uri`가 비어 있을 때 사용할 기본 호스트
 * @param fallback_port `redis_uri`가 비어 있을 때 사용할 기본 포트
 * @param dlq_stream_key `XLEN`으로 조회할 Write-Behind DLQ 스트림 키 이름
 * @param deadline_ms 전체 RESP probe 제한 시간(ms)
 * @return Redis 정상 응답 여부, DLQ 적체 건수, 지연 시간, 비밀이 제거된 오류 설명
 */
RedisProbeResult probe_redis_resp(const std::string& redis_uri,
                                  const std::string& fallback_host,
                                  unsigned short fallback_port,
                                  const std::string& dlq_stream_key,
                                  std::uint32_t deadline_ms = 1500);

/**
 * @brief Redis 키의 남은 TTL을 밀리초(`PTTL`) 단위로 조회합니다.
 * @param redis_uri Redis 연결 URI
 * @param fallback_host `redis_uri`가 비어 있을 때 사용할 기본 호스트
 * @param fallback_port `redis_uri`가 비어 있을 때 사용할 기본 포트
 * @param key TTL을 조회할 Redis 키
 * @param deadline_ms 전체 RESP 제한 시간(ms)
 * @return 남은 TTL(ms, `>= 0`), 만료 없음(`-1`), 키 없음(`-2`), 통신 실패(`-3`)
 */
long long query_redis_pttl_ms(const std::string& redis_uri,
                              const std::string& fallback_host,
                              unsigned short fallback_port,
                              const std::string& key,
                              std::uint32_t deadline_ms = 1000);

/**
 * @brief Write-Behind Pending/DLQ/Dead 스트림 길이와 DLQ 미리보기 항목을 bounded RESP로 조회합니다.
 * @param redis_uri Redis 연결 URI
 * @param fallback_host `redis_uri`가 비어 있을 때 사용할 기본 호스트
 * @param fallback_port `redis_uri`가 비어 있을 때 사용할 기본 포트
 * @param wb_stream 기본 write-behind 스트림 키
 * @param dlq_stream DLQ 스트림 키
 * @param dead_stream 영구 격리(`dead`) 스트림 키
 * @param preview_limit 최대 미리보기 항목 수
 * @param deadline_ms 전체 RESP 제한 시간(ms)
 * @return 스트림 건수 및 DLQ 항목 미리보기를 담은 `WbStreamsSnapshot`
 */
WbStreamsSnapshot inspect_wb_streams_resp(const std::string& redis_uri,
                                          const std::string& fallback_host,
                                          unsigned short fallback_port,
                                          const std::string& wb_stream,
                                          const std::string& dlq_stream,
                                          const std::string& dead_stream,
                                          std::size_t preview_limit = 25,
                                          std::uint32_t deadline_ms = 1500);

/**
 * @brief Write-Behind DLQ 컨슈머 그룹이 없으면 시작 오프셋 `0`으로 생성(`XGROUP CREATE ... 0 MKSTREAM`)합니다.
 * @param redis_uri Redis 연결 URI
 * @param fallback_host `redis_uri`가 비어 있을 때 사용할 기본 호스트
 * @param fallback_port `redis_uri`가 비어 있을 때 사용할 기본 포트
 * @param stream_key DLQ 스트림 키 이름
 * @param group_name 컨슈머 그룹 이름
 * @param deadline_ms 전체 RESP 제한 시간(ms)
 * @return 그룹이 새로 생성되었거나 이미 존재(`BUSYGROUP`)하면 true
 */
bool ensure_stream_group_from_zero_resp(const std::string& redis_uri,
                                        const std::string& fallback_host,
                                        unsigned short fallback_port,
                                        const std::string& stream_key,
                                        const std::string& group_name,
                                        std::uint32_t deadline_ms = 1500);

/**
 * @brief 처리 완료된 DLQ 스트림 항목을 컨슈머 그룹에서 `XACK`하고 스트림에서 `XDEL`합니다.
 * @param redis_uri Redis 연결 URI
 * @param fallback_host `redis_uri`가 비어 있을 때 사용할 기본 호스트
 * @param fallback_port `redis_uri`가 비어 있을 때 사용할 기본 포트
 * @param stream_key DLQ 스트림 키 이름
 * @param group_name 컨슈머 그룹 이름
 * @param entry_id 재처리 완료된 스트림 항목 ID
 * @param deadline_ms 전체 RESP 제한 시간(ms)
 * @return `XACK` 및 `XDEL` 파이프라인이 오류 없이 완료되면 true
 */
bool ack_and_delete_stream_entry_resp(const std::string& redis_uri,
                                      const std::string& fallback_host,
                                      unsigned short fallback_port,
                                      const std::string& stream_key,
                                      const std::string& group_name,
                                      const std::string& entry_id,
                                      std::uint32_t deadline_ms = 1500);

} // namespace dynaxis::admin
