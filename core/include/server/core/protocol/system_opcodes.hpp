// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
/**
 * @file
 * @brief opcode 상수와 transport/runtime 정책 표를 같은 소스에서 자동 생성한 헤더입니다.
 *
 * 상수 선언과 정책 표가 손으로 따로 관리되면 "ID는 바뀌었는데 정책은 예전 값" 같은 드리프트가 생기기 쉽습니다.
 * 이 헤더는 JSON spec 한 곳에서 두 정보를 함께 생성해, 런타임과 문서가 같은 계약을 보도록 만듭니다.
 *
 * @note tools/gen_opcodes.py와 JSON spec에서 다시 생성됩니다. 직접 수정하면 다음 생성 때 덮어써집니다.
 */
#pragma once
#include <cstdint>
#include <string_view>
#include "server/core/protocol/opcode_policy.hpp"

namespace server::core::protocol {
// === system (0x0001..0x000F): 연결 유지, 버전 교환, 공통 오류 응답
static constexpr std::uint16_t MSG_HELLO                = 0x0001; // [s2c] 버전/서버 정보
static constexpr std::uint16_t MSG_PING                 = 0x0002; // [c2s] 하트비트 핑
static constexpr std::uint16_t MSG_PONG                 = 0x0003; // [c2s] 하트비트 퐁
static constexpr std::uint16_t MSG_ERR                  = 0x0004; // [s2c] 에러 응답


/**
 * @brief opcode ID를 사람이 읽을 수 있는 기호 이름으로 변환합니다.
 *
 * 로그, 메트릭, 디버거에서는 숫자보다 `MSG_*` 이름이 훨씬 읽기 쉽습니다. 이 표를 generated header에 함께 두면
 * 런타임 코드가 별도 이름 매핑 테이블을 중복 유지하지 않아도 됩니다.
 *
 * @param id 조회할 opcode ID
 * @return 매칭된 opcode 이름, 미정의 ID면 빈 문자열
 */
inline constexpr std::string_view opcode_name( std::uint16_t id ) noexcept
{
  switch( id )
  {
    case 0x0001: return "MSG_HELLO";
    case 0x0002: return "MSG_PING";
    case 0x0003: return "MSG_PONG";
    case 0x0004: return "MSG_ERR";
    default: return std::string_view{};
  }
}

/**
 * @brief opcode ID에 대한 런타임 정책 메타데이터를 반환합니다.
 *
 * 이 함수는 "어느 세션 상태에서 허용되는가", "어느 실행기에서 처리되는가", "어느 transport/delivery를 기대하는가"를
 * 한 표로 고정합니다. 정책 표를 코드 여러 곳에 흩어 놓으면 새로운 opcode 추가 때 누락이 생기기 쉽습니다.
 *
 * @param id 조회할 opcode ID
 * @return 매칭된 opcode 정책, 미정의 ID면 기본 정책
 */
inline constexpr server::core::protocol::OpcodePolicy opcode_policy( std::uint16_t id ) noexcept
{
  switch( id )
  {
    case 0x0001: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0002: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kBoth, server::core::protocol::DeliveryClass::kReliable, 0};
    case 0x0003: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0004: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    default: return server::core::protocol::default_opcode_policy();
  }
}

} // namespace server::core::protocol

