// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
/**
 * @file
 * @brief Opcode 상수/정책 매핑 자동 생성 헤더입니다.
 * @note tools/gen_opcodes.py로 생성됩니다. 직접 수정하지 마세요.
 */
#pragma once
#include <cstdint>
#include <string_view>
#include "server/core/protocol/opcode_policy.hpp"

namespace server::core::protocol {
// === system (0x0001..0x000F): 코어/시스템 프레임
static constexpr std::uint16_t MSG_HELLO                = 0x0001; // [s2c] 버전/서버 정보
static constexpr std::uint16_t MSG_PING                 = 0x0002; // [c2s] 하트비트 핑
static constexpr std::uint16_t MSG_PONG                 = 0x0003; // [c2s] 하트비트 퐁
static constexpr std::uint16_t MSG_ERR                  = 0x0004; // [s2c] 에러 응답


/**
 * @brief Opcode ID를 사람이 읽을 수 있는 이름으로 변환합니다.
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
 * @brief Opcode ID에 대한 런타임 정책 메타데이터를 반환합니다.
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

