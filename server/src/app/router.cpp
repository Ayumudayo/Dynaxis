#include "server/app/router.hpp"

#include "server/core/net/dispatcher.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/chat/chat_service.hpp"

namespace server::app {

// Gateway/LoadBalancer가 Session을 accept하면 이 함수가 opcode→핸들러 매핑을 등록한다.
// ChatService가 대부분의 메시지를 처리하므로 dispatcher는 단순한 라우팅 테이블 역할을 한다.
void register_routes(server::core::Dispatcher& dispatcher, server::app::chat::ChatService& chat) {
    using server::core::protocol::MSG_PING;
    using server::core::protocol::MSG_PONG;
    using server::core::protocol::MSG_LOGIN_REQ;
    using server::core::protocol::MSG_JOIN_ROOM;
    using server::core::protocol::MSG_CHAT_SEND;
    using server::core::protocol::MSG_LEAVE_ROOM;
    using server::core::protocol::MSG_WHISPER_REQ;
    using server::core::protocol::MSG_ROOMS_REQ;
    using server::core::protocol::MSG_ROOM_USERS_REQ;
    using server::core::protocol::MSG_REFRESH_REQ;

    dispatcher.register_handler(MSG_PING,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_ping(s, payload); });

    dispatcher.register_handler(MSG_LOGIN_REQ,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_login(s, payload); });

    dispatcher.register_handler(MSG_JOIN_ROOM,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_join(s, payload); });

    dispatcher.register_handler(MSG_CHAT_SEND,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_chat_send(s, payload); });

    dispatcher.register_handler(MSG_WHISPER_REQ,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_whisper(s, payload); });

    dispatcher.register_handler(MSG_LEAVE_ROOM,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_leave(s, payload); });

    dispatcher.register_handler(MSG_ROOMS_REQ,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_rooms_request(s, payload); });

    dispatcher.register_handler(MSG_ROOM_USERS_REQ,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_room_users_request(s, payload); });

    dispatcher.register_handler(MSG_REFRESH_REQ,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_refresh_request(s, payload); });
}

} // namespace server::app
