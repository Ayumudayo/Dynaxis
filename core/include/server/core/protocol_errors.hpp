// 공통 에러 코드 정의( MSG_ERR payload의 code 필드 값 )
#pragma once
#include <cstdint>

namespace server::core::protocol::errc {

// 일반 프로토콜 수준
static constexpr std::uint16_t INTERNAL_ERROR        = 0x0001; // 서버 내부 오류
static constexpr std::uint16_t LENGTH_LIMIT_EXCEEDED = 0x0002;
static constexpr std::uint16_t UNKNOWN_MSG_ID        = 0x0003;
static constexpr std::uint16_t INVALID_PAYLOAD       = 0x0007;
static constexpr std::uint16_t NAME_TAKEN            = 0x0100; // 닉네임 중복

// 인증/권한/룸 상태
static constexpr std::uint16_t UNAUTHORIZED          = 0x0101;
static constexpr std::uint16_t NO_ROOM               = 0x0104; // 현재 방 없음
static constexpr std::uint16_t NOT_MEMBER            = 0x0105; // 해당 방 멤버가 아님
static constexpr std::uint16_t ROOM_MISMATCH         = 0x0106; // 지정된 방과 현재 방 불일치

} // namespace server::core::protocol::errc
