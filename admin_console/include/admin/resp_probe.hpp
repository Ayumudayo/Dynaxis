#pragma once

#include <cstdint>
#include <string>

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

} // namespace dynaxis::admin
