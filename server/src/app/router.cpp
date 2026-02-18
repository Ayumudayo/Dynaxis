#include "server/app/router.hpp"

#include "server/core/net/dispatcher.hpp"
#include "server/core/net/session.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/chat/chat_service.hpp"

/**
 * @brief opcode -> ChatService 핸들러 매핑 구현입니다.
 *
 * 네트워크 계층과 비즈니스 계층을 분리하기 위해,
 * 부트 시점에 디스패처 테이블을 한 번 구성하고 런타임엔 조회만 수행합니다.
 */
namespace server::app {

// Gateway나 LoadBalancer가 새 Session을 넘겨주면 opcode → handler 매핑을 모두 등록한다.
// 현재 서버는 ChatService 단일 모듈이 모든 메시지를 처리하므로 dispatcher 테이블만 채우면 된다.
// ChatService가 대부분의 메시지를 처리하므로 dispatcher는 단순한 라우팅 테이블 역할을 한다.
// 각 메시지 ID(opcode)에 대해 어떤 함수가 호출되어야 하는지 정의합니다.
void register_routes(server::core::Dispatcher& dispatcher, server::app::chat::ChatService& chat) {
    using NetSession = server::core::net::Session;
    using server::core::protocol::MSG_PING;
    using server::core::protocol::MSG_PONG;
    using server::protocol::MSG_LOGIN_REQ;
    using server::protocol::MSG_JOIN_ROOM;
    using server::protocol::MSG_CHAT_SEND;
    using server::protocol::MSG_LEAVE_ROOM;
    using server::protocol::MSG_WHISPER_REQ;
    using server::protocol::MSG_ROOMS_REQ;
    using server::protocol::MSG_ROOM_USERS_REQ;
    using server::protocol::MSG_REFRESH_REQ;

    // keep-alive 핸들러: ping payload를 그대로 pong으로 반사한다.
    dispatcher.register_handler(MSG_PING,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_ping(s, payload); });

    // PONG 핸들러: 클라이언트의 응답을 수신하고 무시한다 (RTT 측정 등은 추후 구현)
    dispatcher.register_handler(MSG_PONG,
        [](NetSession&, std::span<const std::uint8_t>) {});

    dispatcher.register_handler(MSG_LOGIN_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_login(s, payload); });

    dispatcher.register_handler(MSG_JOIN_ROOM,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_join(s, payload); });

    dispatcher.register_handler(MSG_CHAT_SEND,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_chat_send(s, payload); });

    dispatcher.register_handler(MSG_WHISPER_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_whisper(s, payload); });

    dispatcher.register_handler(MSG_LEAVE_ROOM,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_leave(s, payload); });

    dispatcher.register_handler(MSG_ROOMS_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_rooms_request(s, payload); });

    dispatcher.register_handler(MSG_ROOM_USERS_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_room_users_request(s, payload); });

    dispatcher.register_handler(MSG_REFRESH_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_refresh_request(s, payload); });
}

} // namespace server::app
