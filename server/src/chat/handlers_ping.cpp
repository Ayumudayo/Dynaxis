// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/storage/redis/client.hpp"
#include <cstdlib>

using namespace server::core;
namespace proto = server::core::protocol;

namespace server::app::chat {

void ChatService::on_ping(Session& s, std::span<const std::uint8_t> payload) {
    // 요청 payload를 그대로 반사하여 PONG 전송
    std::vector<std::uint8_t> body(payload.begin(), payload.end());
    s.async_send(proto::MSG_PONG, body, 0);

    // 경량 heartbeat: 로그인된 사용자에 한해 presence:user:{uid} TTL 갱신
    if (!redis_) return;
    try {
        std::string uid;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(&s)) return;
            auto it = state_.user_uuid.find(&s);
            if (it != state_.user_uuid.end()) uid = it->second;
        }
        if (uid.empty()) return;
        unsigned int ttl = 30;
        if (const char* v = std::getenv("PRESENCE_TTL_SEC")) {
            unsigned long t = std::strtoul(v, nullptr, 10);
            if (t > 0 && t < 3600) ttl = static_cast<unsigned int>(t);
        }
        std::string pfx; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) pfx = p;
        std::string key = pfx + std::string("presence:user:") + uid;
        redis_->setex(key, "1", ttl);
    } catch (...) {
        // no-op: heartbeat 실패는 치명적이지 않음
    }
}

} // namespace server::app::chat

