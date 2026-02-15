// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
#pragma once
#include <cstdint>
#include <string_view>

namespace server::core::protocol {
// === system (0x0001..0x000F): core/system frames
static constexpr std::uint16_t MSG_HELLO                = 0x0001; // [s2c] 버전/서버 정보
static constexpr std::uint16_t MSG_PING                 = 0x0002; // [c2s] heartbeat ping
static constexpr std::uint16_t MSG_PONG                 = 0x0003; // [c2s] heartbeat pong
static constexpr std::uint16_t MSG_ERR                  = 0x0004; // [s2c] 에러 응답


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

} // namespace server::core::protocol

