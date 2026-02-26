#pragma once

#include <cstdint>

/**
 * @brief 프로토콜 프레임/기능 협상에 사용하는 비트 플래그 정의입니다.
 *
 * 프레임 헤더 flags와 HELLO capability를 분리해,
 * 전송 상태(압축/암호화)와 기능 지원 여부를 명확히 구분합니다.
 */
namespace server::core::protocol {

// flags 비트 정의(SEQ/utc_ts_ms32 는 항상 포함되므로 별도 비트 없음)
enum : std::uint16_t {
    FLAG_COMPRESSED = 0x0001,
    FLAG_ENCRYPTED  = 0x0002,
    // 0x0004, 0x0008 은 예약 비트
    FLAG_SELF       = 0x0100, // 브로드캐스트가 발신자에게 에코될 때 사용
};

// capability 비트 정의(MSG_HELLO payload)
enum : std::uint16_t {
    CAP_COMPRESS_SUPP = 0x0001,
    CAP_SENDER_SID    = 0x0002, // 브로드캐스트 payload 에 sender_sid(u32) 포함
    // 나머지 비트는 미사용
};

} // namespace server::core::protocol
