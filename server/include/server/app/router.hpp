#pragma once

#include <cstdint>

namespace server::core { class Dispatcher; }
namespace server::app::chat { class ChatService; }
namespace server::app::fps { class FpsService; }

namespace server::app {

/**
 * @brief opcode 라우팅 테이블을 등록합니다.
 * @param d 핸들러를 등록할 디스패처
 * @param chat 채팅 서비스 구현체
 * @param fps 선택 FPS 앱. nullptr이면 FPS opcode를 등록하지 않습니다.
 */
void register_routes(server::core::Dispatcher& d,
                     server::app::chat::ChatService& chat,
                     server::app::fps::FpsService* fps = nullptr);

} // namespace server::app
