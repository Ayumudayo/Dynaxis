// 자동 생성 파일: tools/gen_opcodes.py에 의해 생성됨
#pragma once
#include <cstdint>

namespace server::protocol {
static constexpr std::uint16_t MSG_LOGIN_REQ            = 0x0010; // 로그인 요청
static constexpr std::uint16_t MSG_LOGIN_RES            = 0x0011; // 로그인 응답
static constexpr std::uint16_t MSG_CHAT_SEND            = 0x0100; // 채팅 전송
static constexpr std::uint16_t MSG_CHAT_BROADCAST       = 0x0101; // 채팅 브로드캐스트
static constexpr std::uint16_t MSG_JOIN_ROOM            = 0x0102; // 룸 입장
static constexpr std::uint16_t MSG_LEAVE_ROOM           = 0x0103; // 룸 퇴장
static constexpr std::uint16_t MSG_WHISPER_REQ          = 0x0104; // 귓속말 요청
static constexpr std::uint16_t MSG_WHISPER_RES          = 0x0105; // 귓속말 응답
static constexpr std::uint16_t MSG_WHISPER_BROADCAST    = 0x0106; // 귓속말 전달
static constexpr std::uint16_t MSG_STATE_SNAPSHOT       = 0x0200; // 상태 스냅샷(방 목록+현재 방 유저)
static constexpr std::uint16_t MSG_ROOM_USERS           = 0x0201; // 특정 방 유저 목록 응답
static constexpr std::uint16_t MSG_ROOMS_REQ            = 0x0202; // 방 목록 요청
static constexpr std::uint16_t MSG_ROOM_USERS_REQ       = 0x0203; // 특정 방 사용자 목록 요청
static constexpr std::uint16_t MSG_REFRESH_REQ          = 0x0204; // 현재 방 스냅샷 요청
static constexpr std::uint16_t MSG_REFRESH_NOTIFY       = 0x0205; // 상태 변경 알림 (클라이언트가 REFRESH_REQ를 보내도록 유도)
} // namespace server::protocol

