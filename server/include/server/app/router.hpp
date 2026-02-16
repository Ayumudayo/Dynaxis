#pragma once

#include <cstdint>

namespace server::core { class Dispatcher; }
namespace server::app::chat { class ChatService; }

namespace server::app {

/**
 * @brief opcode 라우팅 테이블을 등록합니다.
 * @param d 핸들러를 등록할 디스패처
 * @param chat 채팅 서비스 구현체
 */
void register_routes(server::core::Dispatcher& d, server::app::chat::ChatService& chat);

} // namespace server::app
