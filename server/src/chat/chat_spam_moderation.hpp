#pragma once

#include <memory>
#include <string>
#include <vector>

#include "chat_service_state.hpp"

namespace server::app::chat {

struct ChatSendModerationResult {
    std::string sender{"guest"};
    bool sender_is_muted{false};
    bool sender_is_banned{false};
    bool spam_escalated{false};
    bool spam_escalated_to_ban{false};
    std::string moderation_reason;
    std::vector<std::shared_ptr<server::core::net::Session>> penalized_sessions;
};

ChatSendModerationResult evaluate_chat_send_moderation_locked(
    ChatServiceState& state,
    const ChatServiceRuntimeState& runtime,
    const std::shared_ptr<server::core::net::Session>& session);

} // namespace server::app::chat
