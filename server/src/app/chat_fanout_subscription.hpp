#pragma once
#include "server/app/config.hpp"
#include <memory>
namespace server::app::chat { class ChatService; }
namespace server::core::storage::redis { class IRedisClient; }
namespace server::app {
/** @brief Redis 구독을 로컬 room/whisper 전달에 연결합니다. chat 파괴 전에 구독을 종료해야 합니다. */
void start_chat_fanout_subscription(chat::ChatService& chat,
    const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis,
    const ServerConfig& config);
}
