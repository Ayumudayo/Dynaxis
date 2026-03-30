#include "server/chat/chat_service.hpp"

#include "chat_blacklist_state.hpp"
#include "chat_command_authorization.hpp"
#include "chat_room_state.hpp"
#include "chat_service_state.hpp"

#include "server/core/storage/redis/client.hpp"

#include <chrono>
#include <sstream>

namespace server::app::chat {

namespace {

std::uint32_t parse_duration_sec(std::string_view raw,
                                 std::uint32_t fallback,
                                 std::uint32_t min_value,
                                 std::uint32_t max_value) {
    if (raw.empty()) {
        return fallback;
    }

    try {
        const auto parsed = std::stoul(std::string(raw));
        if (parsed < min_value || parsed > max_value) {
            return fallback;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return fallback;
    }
}

} // namespace

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

    if (text.rfind("/invite ", 0) == 0) {
        std::istringstream iss(text.substr(8));
        std::string target_user;
        std::string target_room;
        iss >> target_user;
        iss >> target_room;
        if (target_room.empty()) {
            target_room = current_room;
        }
        if (target_user.empty() || target_room.empty() || target_room == "lobby") {
            send_system_notice(*session_sp, "usage: /invite <user> [room]");
            return true;
        }

        bool allowed = false;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_can_manage_room_locked(impl_->state, impl_->runtime, *session_sp, target_room);
            if (allowed) {
                impl_->state.room_invites[target_room].insert(target_user);
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "invite denied: owner/admin only");
            return true;
        }

        send_system_notice(*session_sp, "invited user: " + target_user + " room=" + target_room);
        return true;
    }

    if (text.rfind("/kick ", 0) == 0) {
        std::istringstream iss(text.substr(6));
        std::string target_user;
        std::string target_room;
        iss >> target_user;
        iss >> target_room;
        if (target_room.empty()) {
            target_room = current_room;
        }
        if (target_user.empty() || target_room.empty() || target_room == "lobby") {
            send_system_notice(*session_sp, "usage: /kick <user> [room]");
            return true;
        }

        bool allowed = false;
        std::vector<std::shared_ptr<Session>> kicked_sessions;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_can_manage_room_locked(impl_->state, impl_->runtime, *session_sp, target_room);

            if (allowed) {
                auto users_it = impl_->state.by_user.find(target_user);
                if (users_it != impl_->state.by_user.end()) {
                    for (auto wit = users_it->second.begin(); wit != users_it->second.end();) {
                        if (auto candidate = wit->lock()) {
                            const auto cur_room_it = impl_->state.cur_room.find(candidate.get());
                            if (cur_room_it != impl_->state.cur_room.end()
                                && cur_room_it->second == target_room) {
                                remove_session_from_room_locked(impl_->state, candidate, target_room, target_user);
                                place_session_in_room_locked(impl_->state, candidate, "lobby", target_user);
                                kicked_sessions.push_back(std::move(candidate));
                            }
                            ++wit;
                        } else {
                            wit = users_it->second.erase(wit);
                        }
                    }
                }
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "kick denied: owner/admin only");
            return true;
        }

        for (auto& kicked : kicked_sessions) {
            send_system_notice(*kicked, "kicked from room: " + target_room);
            send_snapshot(*kicked, "lobby");
        }
        if (!kicked_sessions.empty()) {
            broadcast_refresh(target_room);
            broadcast_refresh("lobby");
        }
        send_system_notice(*session_sp,
                           kicked_sessions.empty() ? "kick target not found in room" : "kick applied");
        return true;
    }

    if (text.rfind("/room remove", 0) == 0) {
        std::istringstream iss(text.substr(12));
        std::string target_room;
        iss >> target_room;
        if (target_room.empty()) {
            target_room = current_room;
        }
        if (target_room.empty() || target_room == "lobby") {
            send_system_notice(*session_sp, "usage: /room remove [room]");
            return true;
        }

        bool allowed = false;
        std::vector<std::shared_ptr<Session>> moved_sessions;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_can_manage_room_locked(impl_->state, impl_->runtime, *session_sp, target_room);

            if (allowed) {
                auto room_it = impl_->state.rooms.find(target_room);
                if (room_it != impl_->state.rooms.end()) {
                    for (auto wit = room_it->second.begin(); wit != room_it->second.end();) {
                        if (auto candidate = wit->lock()) {
                            place_session_in_room_locked(impl_->state, candidate, "lobby", "guest");
                            moved_sessions.push_back(std::move(candidate));
                            ++wit;
                        } else {
                            wit = room_it->second.erase(wit);
                        }
                    }
                    impl_->state.rooms.erase(room_it);
                }
                impl_->state.room_passwords.erase(target_room);
                impl_->state.room_owners.erase(target_room);
                impl_->state.room_invites.erase(target_room);
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "room remove denied: owner/admin only");
            return true;
        }

        for (auto& moved : moved_sessions) {
            send_system_notice(*moved, "room removed: " + target_room);
            send_snapshot(*moved, "lobby");
        }
        if (impl_->runtime.redis) {
            try {
                impl_->runtime.redis->srem("rooms:active", target_room);
                impl_->runtime.redis->del("room:password:" + target_room);
                impl_->runtime.redis->del("room:users:" + target_room);
            } catch (...) {
            }
        }
        broadcast_refresh(target_room);
        broadcast_refresh("lobby");
        send_system_notice(*session_sp, "room removed: " + target_room);
        return true;
    }

    if (text.rfind("/mute ", 0) == 0) {
        std::istringstream iss(text.substr(6));
        std::string target_user;
        std::string duration_raw;
        iss >> target_user;
        iss >> duration_raw;
        if (target_user.empty()) {
            send_system_notice(*session_sp, "usage: /mute <user> [seconds]");
            return true;
        }

        const auto duration_sec = parse_duration_sec(
            duration_raw,
            impl_->runtime.spam_mute_sec,
            5,
            86400);

        bool allowed = false;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
            if (allowed) {
                impl_->state.muted_users[target_user] = {
                    std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec),
                    "muted by admin",
                };
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "mute denied: admin only");
            return true;
        }

        send_system_notice(
            *session_sp,
            "mute applied: user=" + target_user + " duration=" + std::to_string(duration_sec) + "s");
        return true;
    }

    if (text.rfind("/unmute ", 0) == 0) {
        std::istringstream iss(text.substr(8));
        std::string target_user;
        iss >> target_user;
        if (target_user.empty()) {
            send_system_notice(*session_sp, "usage: /unmute <user>");
            return true;
        }

        bool allowed = false;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
            if (allowed) {
                changed = impl_->state.muted_users.erase(target_user) > 0;
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "unmute denied: admin only");
            return true;
        }

        send_system_notice(
            *session_sp,
            changed ? "unmute applied: user=" + target_user : "unmute no-op: user not muted");
        return true;
    }

    if (text.rfind("/ban ", 0) == 0) {
        std::istringstream iss(text.substr(5));
        std::string target_user;
        std::string duration_raw;
        iss >> target_user;
        iss >> duration_raw;
        if (target_user.empty()) {
            send_system_notice(*session_sp, "usage: /ban <user> [seconds]");
            return true;
        }

        const auto duration_sec = parse_duration_sec(
            duration_raw,
            impl_->runtime.spam_ban_sec,
            10,
            604800);

        bool allowed = false;
        std::vector<std::shared_ptr<Session>> banned_sessions;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
            if (allowed) {
                const auto expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
                impl_->state.banned_users[target_user] = {expires_at, "banned by admin"};

                if (const auto ip_it = impl_->state.user_last_ip.find(target_user);
                    ip_it != impl_->state.user_last_ip.end() && !ip_it->second.empty()) {
                    impl_->state.banned_ips[ip_it->second] = expires_at;
                }
                if (const auto hwid_it = impl_->state.user_last_hwid_hash.find(target_user);
                    hwid_it != impl_->state.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                    impl_->state.banned_hwid_hashes[hwid_it->second] = expires_at;
                }

                const auto users_it = impl_->state.by_user.find(target_user);
                if (users_it != impl_->state.by_user.end()) {
                    for (auto wit = users_it->second.begin(); wit != users_it->second.end();) {
                        if (auto candidate = wit->lock()) {
                            banned_sessions.push_back(std::move(candidate));
                            ++wit;
                        } else {
                            wit = users_it->second.erase(wit);
                        }
                    }
                }
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "ban denied: admin only");
            return true;
        }

        for (auto& banned : banned_sessions) {
            send_system_notice(*banned, "temporarily banned");
            banned->stop();
        }
        send_system_notice(
            *session_sp,
            "ban applied: user=" + target_user + " duration=" + std::to_string(duration_sec) + "s");
        return true;
    }

    if (text.rfind("/unban ", 0) == 0) {
        std::istringstream iss(text.substr(7));
        std::string target_user;
        iss >> target_user;
        if (target_user.empty()) {
            send_system_notice(*session_sp, "usage: /unban <user>");
            return true;
        }

        bool allowed = false;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
            if (allowed) {
                changed = impl_->state.banned_users.erase(target_user) > 0;
                if (const auto ip_it = impl_->state.user_last_ip.find(target_user);
                    ip_it != impl_->state.user_last_ip.end() && !ip_it->second.empty()) {
                    changed = impl_->state.banned_ips.erase(ip_it->second) > 0 || changed;
                }
                if (const auto hwid_it = impl_->state.user_last_hwid_hash.find(target_user);
                    hwid_it != impl_->state.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                    changed = impl_->state.banned_hwid_hashes.erase(hwid_it->second) > 0 || changed;
                }
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "unban denied: admin only");
            return true;
        }

        send_system_notice(
            *session_sp,
            changed ? "unban applied: user=" + target_user : "unban no-op: user not banned");
        return true;
    }

    if (text.rfind("/gkick ", 0) == 0) {
        std::istringstream iss(text.substr(7));
        std::string target_user;
        iss >> target_user;
        if (target_user.empty()) {
            send_system_notice(*session_sp, "usage: /gkick <user>");
            return true;
        }

        bool allowed = false;
        std::vector<std::shared_ptr<Session>> kicked_sessions;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
            if (allowed) {
                const auto users_it = impl_->state.by_user.find(target_user);
                if (users_it != impl_->state.by_user.end()) {
                    for (auto wit = users_it->second.begin(); wit != users_it->second.end();) {
                        if (auto candidate = wit->lock()) {
                            kicked_sessions.push_back(std::move(candidate));
                            ++wit;
                        } else {
                            wit = users_it->second.erase(wit);
                        }
                    }
                }
            }
        }

        if (!allowed) {
            send_system_notice(*session_sp, "global kick denied: admin only");
            return true;
        }

        for (auto& kicked : kicked_sessions) {
            send_system_notice(*kicked, "disconnected by administrator");
            kicked->stop();
        }
        send_system_notice(
            *session_sp,
            kicked_sessions.empty() ? "global kick target not found" : "global kick applied");
        return true;
    }

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
