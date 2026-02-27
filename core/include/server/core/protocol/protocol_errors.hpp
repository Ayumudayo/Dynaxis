#pragma once

#include <cstdint>

/**
 * @brief `MSG_ERR` payload의 code 필드에서 사용하는 표준 오류 코드 집합입니다.
 *
 * 서버/클라이언트가 동일한 수치 코드를 공유해야
 * 운영 로그, 모니터링, 사용자 메시지 매핑을 일관되게 유지할 수 있습니다.
 */
namespace server::core::protocol::errc {

// MSG_ERR payload 의 code 필드에 사용되는 오류 코드 정의
static constexpr std::uint16_t INTERNAL_ERROR        = 0x0001; // 내부 처리 실패
static constexpr std::uint16_t LENGTH_LIMIT_EXCEEDED = 0x0002; // payload 길이 초과
static constexpr std::uint16_t UNKNOWN_MSG_ID        = 0x0003; // 등록되지 않은 메시지
static constexpr std::uint16_t INVALID_PAYLOAD       = 0x0007; // payload 파싱 실패
static constexpr std::uint16_t SERVER_BUSY           = 0x0008; // 서버 과부하(재시도 가능)
static constexpr std::uint16_t UNSUPPORTED_VERSION   = 0x0009; // 프로토콜 버전 협상 실패
static constexpr std::uint16_t NAME_TAKEN            = 0x0100; // 닉네임 중복

// 인증/권한/방 관련 오류
static constexpr std::uint16_t UNAUTHORIZED          = 0x0101;
static constexpr std::uint16_t FORBIDDEN             = 0x0102;
static constexpr std::uint16_t NO_ROOM               = 0x0104; // 대상 방이 존재하지 않음
static constexpr std::uint16_t NOT_MEMBER            = 0x0105; // 해당 방 구성원이 아님
static constexpr std::uint16_t ROOM_MISMATCH         = 0x0106; // 세션이 속한 방과 요청 정보가 다름

} // namespace server::core::protocol::errc
