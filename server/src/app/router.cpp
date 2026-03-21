#include "server/app/router.hpp"

#include "server/core/net/dispatcher.hpp"
#include "server/core/protocol/opcode_policy.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/util/log.hpp"
#include "server/fps/fps_service.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/chat/chat_service.hpp"

#include <utility>

/**
 * @brief opcode -> ChatService 핸들러 매핑 구현입니다.
 *
 * 네트워크 계층과 비즈니스 계층을 분리하기 위해,
 * 부트 시점에 디스패처 테이블을 한 번 구성하고 런타임엔 조회만 수행합니다.
 * 등록 지점을 한 파일에 모아 두면 "어떤 메시지가 어떤 정책으로 어디에 연결되는가"를 한눈에 점검할 수 있습니다.
 * 이 표가 여러 파일로 흩어지면 transport policy 누락이나 opcode 중복을 코드 리뷰에서 발견하기가 어려워집니다.
 */
namespace server::app {

// 이 표는 "dispatcher가 알아야 하는 마지막 app-local 사실"만 담는다.
// 세부 비즈니스 규칙까지 이 파일로 끌어오면 opcode 표가 곧 서비스 구현이 되어 버려 유지보수가 급격히 나빠진다.
void register_routes(server::core::Dispatcher& dispatcher,
                     server::app::chat::ChatService& chat,
                     server::app::fps::FpsService& fps) {
    using NetSession = server::app::chat::ChatService::NetSession;
    using server::core::protocol::TransportMask;
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
    using server::protocol::MSG_FPS_INPUT;

    auto register_core = [&dispatcher](std::uint16_t msg_id, auto&& handler) {
        const auto policy = server::core::protocol::opcode_policy(msg_id);
        if (policy.transport == TransportMask::kNone) {
            server::core::log::warn("core opcode policy transport=none for msg_id=" + std::to_string(msg_id));
        }
        dispatcher.register_handler(msg_id, std::forward<decltype(handler)>(handler), policy);
    };

    auto register_game = [&dispatcher](std::uint16_t msg_id, auto&& handler) {
        const auto policy = server::protocol::opcode_policy(msg_id);
        if (policy.transport == TransportMask::kNone) {
            server::core::log::warn("game opcode policy transport=none for msg_id=" + std::to_string(msg_id));
        }
        dispatcher.register_handler(msg_id, std::forward<decltype(handler)>(handler), policy);
    };

    // keep-alive는 가장 싼 왕복 확인 경로이므로 라우터 단계에서도 눈에 띄게 남겨 둔다.
    register_core(MSG_PING,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_ping(s, payload); });

    // PONG은 현재 상태 변이를 만들지 않으므로 빈 핸들러로 두되, 등록 자체는 명시적으로 남겨 프로토콜 표를 완성한다.
    register_core(MSG_PONG,
        [](NetSession&, std::span<const std::uint8_t>) {});

    register_game(MSG_LOGIN_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_login(s, payload); });

    register_game(MSG_JOIN_ROOM,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_join(s, payload); });

    register_game(MSG_CHAT_SEND,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_chat_send(s, payload); });

    register_game(MSG_WHISPER_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_whisper(s, payload); });

    register_game(MSG_LEAVE_ROOM,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_leave(s, payload); });

    register_game(MSG_ROOMS_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_rooms_request(s, payload); });

    register_game(MSG_ROOM_USERS_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_room_users_request(s, payload); });

    register_game(MSG_REFRESH_REQ,
        [&chat](NetSession& s, std::span<const std::uint8_t> payload) { chat.on_refresh_request(s, payload); });

    register_game(MSG_FPS_INPUT,
        [&fps](NetSession& s, std::span<const std::uint8_t> payload) { fps.on_input(s, payload); });
}

} // namespace server::app
