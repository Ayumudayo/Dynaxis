// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
#pragma once
#include <cstdint>

namespace server::core::protocol {
static constexpr std::uint16_t MSG_HELLO                = 0x0001; // 버전/서버 정보
static constexpr std::uint16_t MSG_PING                 = 0x0002; // heartbeat ping
static constexpr std::uint16_t MSG_PONG                 = 0x0003; // heartbeat pong
static constexpr std::uint16_t MSG_ERR                  = 0x0004; // 에러 응답
static constexpr std::uint16_t MSG_LOGIN_REQ            = 0x0010; // 로그인 요청
static constexpr std::uint16_t MSG_LOGIN_RES            = 0x0011; // 로그인 응답
static constexpr std::uint16_t MSG_CHAT_SEND            = 0x0100; // 채팅 전송
static constexpr std::uint16_t MSG_CHAT_BROADCAST       = 0x0101; // 채팅 브로드캐스트
static constexpr std::uint16_t MSG_JOIN_ROOM            = 0x0102; // 룸 입장
static constexpr std::uint16_t MSG_LEAVE_ROOM           = 0x0103; // 룸 퇴장
static constexpr std::uint16_t MSG_STATE_SNAPSHOT       = 0x0200; // 상태 스냅샷(방 목록+현재 방 유저)
static constexpr std::uint16_t MSG_ROOM_USERS           = 0x0201; // 특정 방 유저 목록 응답
} // namespace server::core::protocol

