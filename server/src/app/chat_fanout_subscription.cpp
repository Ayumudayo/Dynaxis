#include "chat_fanout_subscription.hpp"
#include "server_runtime_state.hpp"
#include "server/chat/chat_service.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
#include <stdexcept>
namespace server::app {
/** @brief 원격 메시지만 전달해 Redis fanout의 자기 에코와 재발행 루프를 막습니다. */
void start_chat_fanout_subscription(chat::ChatService& chat,
    const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis,
    const ServerConfig& config) {
    if (!redis || !config.use_redis_pubsub) { return; }
    const auto prefix = config.redis_channel_prefix + "fanout:";
    if (!redis->start_psubscribe(prefix + "*",
        [&chat, prefix, origin = config.gateway_id](const std::string& channel, const std::string& message) {
            if (!message.starts_with("gw=")) { return; }
            const auto newline = message.find('\n');
            if (newline == std::string::npos || message.substr(3, newline - 3) == origin) { return; }
            const auto refresh = prefix + "refresh:";
            const auto room = prefix + "room:";
            const std::vector<std::uint8_t> body(message.begin() + newline + 1, message.end());
            if (channel.starts_with(refresh)) {
                chat.broadcast_refresh_local(channel.substr(refresh.size()));
            } else if (channel.starts_with(room)) {
                chat.broadcast_room(channel.substr(room.size()), body);
            } else if (channel == prefix + "whisper") {
                chat.deliver_remote_whisper(body);
            } else { return; }
            record_bootstrap_subscribe();
        })) {
        throw std::runtime_error("chat fanout subscription failed");
    }
}
}
