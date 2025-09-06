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
} // namespace server::core::protocol

