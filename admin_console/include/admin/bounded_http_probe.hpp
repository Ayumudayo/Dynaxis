#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dynaxis::admin {

/**
 * @brief bounded HTTP GET probe 실행 결과입니다.
 */
struct BoundedHttpProbeResult {
    bool ok{false};
    int status_code{0};
    std::uint32_t latency_ms{0};
    std::string body;
    std::string error;
};

/**
 * @brief 개별 timeout·헤더 상한·본문 상한을 적용해 HTTP GET probe를 수행합니다.
 *
 * 느리거나 무응답인 대상 소켓이 제어면 전체를 정지시키지 않도록 `deadline_ms` 경과 시 즉시 소켓을 취소합니다.
 *
 * @param host 대상 호스트명 또는 IP 주소
 * @param port 대상 TCP 포트 번호
 * @param target HTTP 요청 경로(예: `"/healthz"`, `"/readyz"`, `"/metrics"`)
 * @param deadline_ms 전체 연결·요청·응답 제한 시간(ms)
 * @param max_header_bytes 허용할 최대 HTTP 응답 헤더 크기(바이트)
 * @param max_body_bytes 허용할 최대 HTTP 응답 본문 크기(바이트)
 * @return HTTP 상태 코드, 지연 시간, 제한된 본문, 비밀이 제거된 오류 메시지
 */
BoundedHttpProbeResult bounded_http_get(const std::string& host,
                                        unsigned short port,
                                        const std::string& target,
                                        std::uint32_t deadline_ms = 1500,
                                        std::size_t max_header_bytes = 16 * 1024,
                                        std::size_t max_body_bytes = 256 * 1024);

} // namespace dynaxis::admin
