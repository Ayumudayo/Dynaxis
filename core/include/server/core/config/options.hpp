#pragma once

#include <cstddef>

namespace server::core {

/**
 * @brief TCP 세션 동작 파라미터를 묶은 설정 구조체입니다.
 *
 * 왜 필요한가?
 * - 네트워크 보호 한계(최대 payload, 송신 큐 상한)와 타이머(heartbeat/read/write timeout)를
 *   코드에 하드코딩하지 않고 환경별로 조정할 수 있어야 운영 튜닝이 가능합니다.
 */
struct SessionOptions {
    std::size_t recv_max_payload = 32 * 1024;   ///< 수신 payload 최대 크기(바이트, 0이면 제한 없음)
    std::size_t send_queue_max   = 256 * 1024;  ///< 송신 큐 누적량 상한(바이트, 0이면 제한 없음)
    unsigned    heartbeat_interval_ms = 10'000; ///< heartbeat 주기(ms, 0이면 heartbeat 비활성)
    unsigned    read_timeout_ms       = 15'000; ///< 수신 타임아웃(ms, 0이면 감시하지 않음)
    unsigned    write_timeout_ms      = 15'000; ///< 송신 타임아웃(ms, 0이면 감시하지 않음)
};

} // namespace server::core
