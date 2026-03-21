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

namespace server::protocol {
// === auth (0x0010..0x001F): 로그인, 버전 확인, 바인딩 준비
static constexpr std::uint16_t MSG_LOGIN_REQ            = 0x0010; // [c2s] 로그인 요청
static constexpr std::uint16_t MSG_LOGIN_RES            = 0x0011; // [s2c] 로그인 응답
static constexpr std::uint16_t MSG_UDP_BIND_REQ         = 0x0012; // [c2s] UDP 바인딩 요청
static constexpr std::uint16_t MSG_UDP_BIND_RES         = 0x0013; // [s2c] UDP 바인딩 응답/티켓

// === chat (0x0100..0x01FF): 룸 이동, 채팅, 귓속말
static constexpr std::uint16_t MSG_CHAT_SEND            = 0x0100; // [c2s] 채팅 전송
static constexpr std::uint16_t MSG_CHAT_BROADCAST       = 0x0101; // [s2c] 채팅 브로드캐스트
static constexpr std::uint16_t MSG_JOIN_ROOM            = 0x0102; // [c2s] 룸 입장
static constexpr std::uint16_t MSG_LEAVE_ROOM           = 0x0103; // [c2s] 룸 퇴장
static constexpr std::uint16_t MSG_WHISPER_REQ          = 0x0104; // [c2s] 귓속말 요청
static constexpr std::uint16_t MSG_WHISPER_RES          = 0x0105; // [s2c] 귓속말 응답
static constexpr std::uint16_t MSG_WHISPER_BROADCAST    = 0x0106; // [s2c] 귓속말 전달

// === state (0x0200..0x02FF): 스냅샷, refresh, FPS 상태 전송
static constexpr std::uint16_t MSG_STATE_SNAPSHOT       = 0x0200; // [s2c] 상태 스냅샷(방 목록+현재 방 유저)
static constexpr std::uint16_t MSG_ROOM_USERS           = 0x0201; // [s2c] 특정 방 유저 목록 응답
static constexpr std::uint16_t MSG_ROOMS_REQ            = 0x0202; // [c2s] 방 목록 요청
static constexpr std::uint16_t MSG_ROOM_USERS_REQ       = 0x0203; // [c2s] 특정 방 사용자 목록 요청
static constexpr std::uint16_t MSG_REFRESH_REQ          = 0x0204; // [c2s] 현재 방 스냅샷 요청
static constexpr std::uint16_t MSG_REFRESH_NOTIFY       = 0x0205; // [s2c] 상태 변경 알림 (클라이언트가 REFRESH_REQ를 보내도록 유도)
static constexpr std::uint16_t MSG_FPS_INPUT            = 0x0206; // [c2s] FPS fixed-step 입력 프레임
static constexpr std::uint16_t MSG_FPS_STATE_SNAPSHOT   = 0x0207; // [s2c] FPS authoritative 상태 스냅샷
static constexpr std::uint16_t MSG_FPS_STATE_DELTA      = 0x0208; // [s2c] FPS authoritative 상태 델타


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
    case 0x0010: return "MSG_LOGIN_REQ";
    case 0x0011: return "MSG_LOGIN_RES";
    case 0x0012: return "MSG_UDP_BIND_REQ";
    case 0x0013: return "MSG_UDP_BIND_RES";
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
    case 0x0206: return "MSG_FPS_INPUT";
    case 0x0207: return "MSG_FPS_STATE_SNAPSHOT";
    case 0x0208: return "MSG_FPS_STATE_DELTA";
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
    case 0x0010: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0011: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 0};
    case 0x0012: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kUdp, server::core::protocol::DeliveryClass::kReliable, 0};
    case 0x0013: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kBoth, server::core::protocol::DeliveryClass::kReliable, 0};
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
    case 0x0206: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAuthenticated, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kBoth, server::core::protocol::DeliveryClass::kUnreliableSequenced, 1};
    case 0x0207: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kTcp, server::core::protocol::DeliveryClass::kReliableOrdered, 1};
    case 0x0208: return server::core::protocol::OpcodePolicy{server::core::protocol::SessionStatus::kAny, server::core::protocol::ProcessingPlace::kInline, server::core::protocol::TransportMask::kBoth, server::core::protocol::DeliveryClass::kUnreliableSequenced, 1};
    default: return server::core::protocol::default_opcode_policy();
  }
}

} // namespace server::protocol

