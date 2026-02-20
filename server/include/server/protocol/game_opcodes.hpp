// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
#pragma once
#include <cstdint>
#include <string_view>
#include "server/core/protocol/opcode_policy.hpp"

namespace server::protocol {
// === auth (0x0010..0x001F): login/auth
static constexpr std::uint16_t MSG_LOGIN_REQ            = 0x0010; // [c2s] 로그인 요청
static constexpr std::uint16_t MSG_LOGIN_RES            = 0x0011; // [s2c] 로그인 응답

// === chat (0x0100..0x01FF): room chat
static constexpr std::uint16_t MSG_CHAT_SEND            = 0x0100; // [c2s] 채팅 전송
static constexpr std::uint16_t MSG_CHAT_BROADCAST       = 0x0101; // [s2c] 채팅 브로드캐스트
static constexpr std::uint16_t MSG_JOIN_ROOM            = 0x0102; // [c2s] 룸 입장
static constexpr std::uint16_t MSG_LEAVE_ROOM           = 0x0103; // [c2s] 룸 퇴장
static constexpr std::uint16_t MSG_WHISPER_REQ          = 0x0104; // [c2s] 귓속말 요청
static constexpr std::uint16_t MSG_WHISPER_RES          = 0x0105; // [s2c] 귓속말 응답
static constexpr std::uint16_t MSG_WHISPER_BROADCAST    = 0x0106; // [s2c] 귓속말 전달

// === state (0x0200..0x02FF): snapshot/refresh
static constexpr std::uint16_t MSG_STATE_SNAPSHOT       = 0x0200; // [s2c] 상태 스냅샷(방 목록+현재 방 유저)
static constexpr std::uint16_t MSG_ROOM_USERS           = 0x0201; // [s2c] 특정 방 유저 목록 응답
static constexpr std::uint16_t MSG_ROOMS_REQ            = 0x0202; // [c2s] 방 목록 요청
static constexpr std::uint16_t MSG_ROOM_USERS_REQ       = 0x0203; // [c2s] 특정 방 사용자 목록 요청
static constexpr std::uint16_t MSG_REFRESH_REQ          = 0x0204; // [c2s] 현재 방 스냅샷 요청
static constexpr std::uint16_t MSG_REFRESH_NOTIFY       = 0x0205; // [s2c] 상태 변경 알림 (클라이언트가 REFRESH_REQ를 보내도록 유도)


inline constexpr std::string_view opcode_name( std::uint16_t id ) noexcept
{
  switch( id )
  {
    case 0x0010: return "MSG_LOGIN_REQ";
    case 0x0011: return "MSG_LOGIN_RES";
    case 0x0100: return "MSG_CHAT_SEND";
    case 0x0101: return "MSG_CHAT_BROADCAST";
    case 0x0102: return "MSG_JOIN_ROOM";
    case 0x0103: return "MSG_LEAVE_ROOM";
    case 0x0104: return "MSG_WHISPER_REQ";
    case 0x0105: return "MSG_WHISPER_RES";
    case 0x0106: return "MSG_WHISPER_BROADCAST";
    case 0x0200: return "MSG_STATE_SNAPSHOT";
    case 0x0201: return "MSG_ROOM_USERS";
    case 0x0202: return "MSG_ROOMS_REQ";
    case 0x0203: return "MSG_ROOM_USERS_REQ";
    case 0x0204: return "MSG_REFRESH_REQ";
    case 0x0205: return "MSG_REFRESH_NOTIFY";
    default: return std::string_view{};
  }
}

inline constexpr server::core::protocol::OpcodePolicy opcode_policy( std::uint16_t id ) noexcept
{
  switch( id )
  {
    case 0x0010: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0011: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0100: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0101: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0102: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0103: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0104: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0105: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0106: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0200: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0201: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0202: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0203: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0204: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0205: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    default: return server::core::protocol::default_opcode_policy();
  }
}

} // namespace server::protocol

