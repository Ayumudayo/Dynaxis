#include "server/chat/chat_service.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/storage/redis/client.hpp"
#include <cstdlib>

namespace server::app::chat {

void ChatService::on_ping(server::core::Session& s, std::span<const std::uint8_t> payload) {
    namespace proto = server::core::protocol;
    std::vector<std::uint8_t> response(payload.begin(), payload.end());
    s.async_send(proto::MSG_PONG, response);
    try {
        std::string uid;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(&s)) return;
            auto it = state_.user_uuid.find(&s);
            if (it != state_.user_uuid.end()) uid = it->second;
        }
        if (uid.empty()) return;
        touch_user_presence(uid);
    } catch (...) {
        // 네트워크 hiccup 등으로 실패해도 치명적이지 않으므로 조용히 무시한다.
    }
}

} // namespace server::app::chat

