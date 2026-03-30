#include "chat_spam_moderation.hpp"

#include <chrono>

#include "chat_command_authorization.hpp"

namespace server::app::chat {

ChatSendModerationResult evaluate_chat_send_moderation_locked(
    ChatServiceState& state,
    const ChatServiceRuntimeState& runtime,
    const std::shared_ptr<server::core::net::Session>& session) {
    ChatSendModerationResult result{};
    result.sender = actor_name_locked(state, *session);
    if (result.sender == "guest") {
        return result;
    }

    const auto now = std::chrono::steady_clock::now();

    if (auto muted_it = state.muted_users.find(result.sender); muted_it != state.muted_users.end()) {
        if (muted_it->second.expires_at <= now) {
            state.muted_users.erase(muted_it);
        } else {
            result.sender_is_muted = true;
            result.moderation_reason =
                muted_it->second.reason.empty() ? "temporarily muted" : muted_it->second.reason;
        }
    }

    if (auto banned_it = state.banned_users.find(result.sender); banned_it != state.banned_users.end()) {
        if (banned_it->second.expires_at <= now) {
            state.banned_users.erase(banned_it);
        } else {
            result.sender_is_banned = true;
            result.moderation_reason =
                banned_it->second.reason.empty() ? "temporarily banned" : banned_it->second.reason;
        }
    }

    if (result.sender_is_muted || result.sender_is_banned) {
        return result;
    }

    auto& events = state.spam_events[result.sender];
    const auto cutoff = now - std::chrono::seconds(runtime.spam_window_sec);
    while (!events.empty() && events.front() < cutoff) {
        events.pop_front();
    }
    events.push_back(now);

    if (events.size() <= runtime.spam_message_threshold) {
        return result;
    }

    events.clear();
    const auto violations = ++state.spam_violations[result.sender];
    if (violations >= runtime.spam_ban_violation_threshold) {
        result.spam_escalated = true;
        result.spam_escalated_to_ban = true;
        result.moderation_reason = "temporarily banned for repeated spam";
        const auto expires_at = now + std::chrono::seconds(runtime.spam_ban_sec);
        state.banned_users[result.sender] = {expires_at, result.moderation_reason};

        if (auto ip_it = state.user_last_ip.find(result.sender);
            ip_it != state.user_last_ip.end() && !ip_it->second.empty()) {
            state.banned_ips[ip_it->second] = expires_at;
        }
        if (auto hwid_it = state.user_last_hwid_hash.find(result.sender);
            hwid_it != state.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
            state.banned_hwid_hashes[hwid_it->second] = expires_at;
        }

        if (auto users_it = state.by_user.find(result.sender); users_it != state.by_user.end()) {
            for (auto wit = users_it->second.begin(); wit != users_it->second.end();) {
                if (auto candidate = wit->lock()) {
                    result.penalized_sessions.push_back(std::move(candidate));
                    ++wit;
                } else {
                    wit = users_it->second.erase(wit);
                }
            }
        }
        return result;
    }

    result.spam_escalated = true;
    result.moderation_reason = "temporarily muted for spam";
    state.muted_users[result.sender] = {
        now + std::chrono::seconds(runtime.spam_mute_sec),
        result.moderation_reason,
    };
    return result;
}

} // namespace server::app::chat
