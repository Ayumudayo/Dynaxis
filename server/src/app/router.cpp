#include "server/app/router.hpp"

#include "server/core/net/dispatcher.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/chat/chat_service.hpp"

namespace server::app {

void register_routes(server::core::Dispatcher& dispatcher, server::app::chat::ChatService& chat) {
    using server::core::protocol::MSG_PING;
    using server::core::protocol::MSG_PONG;
    using server::core::protocol::MSG_LOGIN_REQ;
    using server::core::protocol::MSG_JOIN_ROOM;
    using server::core::protocol::MSG_CHAT_SEND;
    using server::core::protocol::MSG_LEAVE_ROOM;

    dispatcher.register_handler(MSG_PING,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_ping(s, payload); });

    dispatcher.register_handler(MSG_LOGIN_REQ,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_login(s, payload); });

    dispatcher.register_handler(MSG_JOIN_ROOM,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_join(s, payload); });

    dispatcher.register_handler(MSG_CHAT_SEND,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_chat_send(s, payload); });

    dispatcher.register_handler(MSG_LEAVE_ROOM,
        [&chat](server::core::Session& s, std::span<const std::uint8_t> payload) { chat.on_leave(s, payload); });
}

} // namespace server::app
