#include "server/chat/chat_service.hpp"
#include "chat_service_state.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/storage/redis/client.hpp"
#include <cstdlib>

/**
 * @brief ping/pong keepalive 핸들러 구현입니다.
 *
 * 왕복 연결 확인과 함께 presence TTL을 갱신해,
 * 네트워크가 살아 있는 사용자만 온라인 집계에 반영되도록 유지합니다.
 */
namespace server::app::chat {

void ChatService::on_ping(ChatService::NetSession& s, std::span<const std::uint8_t> payload) {
    namespace proto = server::core::protocol;
    std::vector<std::uint8_t> response(payload.begin(), payload.end());
    auto session = s.shared_from_this();
    if (!enqueue_session(session, [this, session, response = std::move(response)] {
        auto& s = *session;
        s.async_send(proto::MSG_PONG, response);
        try {
            std::string uid;
            {
                std::lock_guard<std::mutex> lk(impl_->state.mu);
                if (!impl_->state.authed.count(&s)) return;
                auto it = impl_->state.user_uuid.find(&s);
                if (it != impl_->state.user_uuid.end()) uid = it->second;
            }
            if (uid.empty()) return;
            touch_user_presence(uid);
        } catch (...) {
            // ping 경로의 presence 갱신 실패는 치명적이지 않으므로 조용히 무시한다.
            // keepalive 하나 실패 때문에 세션 전체를 끊기보다, 다음 tick/핑에서 다시 회복할 기회를 주는 편이 낫다.
        }
    })) s.send_error(proto::errc::SERVER_BUSY, "server busy");
}

} // namespace server::app::chat

