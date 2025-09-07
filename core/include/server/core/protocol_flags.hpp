#pragma once

#include <cstdint>

namespace server::core::protocol {

// flags 비트 정의(고정 헤더에 seq/utc_ts_ms32가 항상 포함되므로 관련 플래그는 제거)
enum : std::uint16_t {
    FLAG_COMPRESSED  = 0x0001,
    FLAG_ENCRYPTED   = 0x0002,
    // 0x0004, 0x0008 예약(이전 SEQ/TS 플래그는 폐지)
    FLAG_SELF        = 0x0100, // 브로드캐스트가 자신의 에코임을 나타냄
};

// capability 비트 정의(MSG_HELLO payload)
enum : std::uint16_t {
    CAP_COMPRESS_SUPP = 0x0001,
    CAP_SENDER_SID    = 0x0002, // 브로드캐스트에 sender_sid(u32) 포함
    // seq/utc_ts_ms32는 항상 지원/포함
};

} // namespace server::core::protocol
