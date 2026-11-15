#include "server/chat/chat_service.hpp"

#include "chat_blacklist_state.hpp"
#include "chat_command_authorization.hpp"
#include "chat_room_state.hpp"
#include "chat_service_state.hpp"

#include "server/core/storage/redis/client.hpp"

#include <chrono>
#include <sstream>

namespace server::app::chat {

bool ChatService::try_handle_slash_command(std::shared_ptr<Session> session_sp,
                                           const std::string& current_room,
                                           const std::string& text) {
    if (text.empty() || text[0] != '/') {
        return false;
    }

    if (text == "/refresh") {
        handle_refresh(std::move(session_sp));
        return true;
    }

    if (text == "/rooms") {
        send_rooms_list(*session_sp);
        return true;
    }

    if (text.rfind("/who", 0) == 0) {
        std::string target = current_room;
        if (text.size() > 4) {
            const auto pos = text.find_first_not_of(' ', 4);
            if (pos != std::string::npos) {
                target = text.substr(pos);
            }
        }
        send_room_users(*session_sp, target);
        return true;
    }

    if (text.rfind("/whisper ", 0) == 0 || text.rfind("/w ", 0) == 0) {
        const bool long_form = text.rfind("/whisper ", 0) == 0;
        std::string args = text.substr(long_form ? 9 : 3);
        const auto leading = args.find_first_not_of(' ');
        if (leading != std::string::npos && leading > 0) {
            args.erase(0, leading);
        }

        const auto spc = args.find(' ');
        if (spc == std::string::npos) {
            send_system_notice(*session_sp, "usage: /whisper <user> <message>");
            send_whisper_result(*session_sp, false, "invalid payload");
            return true;
        }

        std::string target_user = args.substr(0, spc);
        std::string whisper_text = args.substr(spc + 1);
        const auto msg_leading = whisper_text.find_first_not_of(' ');
        if (msg_leading != std::string::npos && msg_leading > 0) {
            whisper_text.erase(0, msg_leading);
        }
        if (target_user.empty() || whisper_text.empty()) {
            send_system_notice(*session_sp, "usage: /whisper <user> <message>");
            send_whisper_result(*session_sp, false, "invalid payload");
            return true;
        }

        dispatch_whisper(std::move(session_sp), target_user, whisper_text);
        return true;
    }

    if (handle_shared_room_command(session_sp, current_room, text)) return true;

    if (text.rfind("/block ", 0) == 0 || text.rfind("/unblock ", 0) == 0
        || text.rfind("/blacklist", 0) == 0) {
        std::string actor;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            actor = actor_name_locked(impl_->state, *session_sp);
        }
        if (actor.empty() || actor == "guest") {
            send_system_notice(*session_sp, "blacklist denied: login required");
            return true;
        }

        const auto command = parse_blacklist_command(text);
        if (command.kind == BlacklistCommandKind::list) {
            std::vector<std::string> blocked_users;
            {
                std::lock_guard<std::mutex> lk(impl_->state.mu);
                blocked_users = collect_blacklisted_users_locked(impl_->state, actor);
            }
            if (blocked_users.empty()) {
                send_system_notice(*session_sp, "blacklist: (empty)");
            } else {
                std::string joined;
                for (std::size_t i = 0; i < blocked_users.size(); ++i) {
                    if (i != 0) {
                        joined += ", ";
                    }
                    joined += blocked_users[i];
                }
                send_system_notice(*session_sp, "blacklist: " + joined);
            }
            return true;
        }

        if (command.kind == BlacklistCommandKind::invalid || command.target_user.empty()) {
            send_system_notice(*session_sp, "usage: /blacklist <add|remove|list> [user]");
            return true;
        }

        if (command.target_user == actor) {
            send_system_notice(*session_sp, "blacklist denied: cannot target yourself");
            return true;
        }

        if (command.kind == BlacklistCommandKind::add) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(impl_->state.mu);
                changed = add_blacklist_entry_locked(impl_->state, actor, command.target_user);
            }
            persist_shared_blacklist(actor);
            send_system_notice(
                *session_sp,
                changed ? "blacklist add: " + command.target_user : "blacklist add no-op");
            return true;
        }

        if (command.kind == BlacklistCommandKind::remove) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(impl_->state.mu);
                changed = remove_blacklist_entry_locked(impl_->state, actor, command.target_user);
            }
            persist_shared_blacklist(actor);
            send_system_notice(
                *session_sp,
                changed ? "blacklist remove: " + command.target_user : "blacklist remove no-op");
            return true;
        }

        send_system_notice(*session_sp, "usage: /blacklist <add|remove|list> [user]");
        return true;
    }

    return false;
}

} // namespace server::app::chat
