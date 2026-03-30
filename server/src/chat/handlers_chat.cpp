
#include "server/chat/chat_service.hpp"
#include "chat_blacklist_state.hpp"
#include "chat_command_authorization.hpp"
#include "chat_room_state.hpp"
#include "chat_spam_moderation.hpp"
#include "chat_service_private_access.hpp"
#include "chat_service_state.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// 저장소 연동 헤더
#include "server/storage/connection_pool.hpp"
#include "server/core/storage/redis/client.hpp"
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <sstream>

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

/**
 * @brief 채팅/귓속말 핸들러 구현입니다.
 *
 * slash command, 권한 검사, 브로드캐스트, write-behind 발행을
 * 작업 큐에서 순차 처리해 데이터 경합과 I/O 스레드 블로킹을 줄입니다.
 */
namespace server::app::chat {

// -----------------------------------------------------------------------------
// 귓속말(Whisper) 처리 핸들러
// -----------------------------------------------------------------------------
// 귓속말은 단순 브로드캐스트보다 상태 조회가 많다.
// 상대 사용자 위치, 차단 상태, 현재 세션 유효성을 함께 봐야 하므로 별도 경로로 다룬다.

void ChatService::on_whisper(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string target;
    std::string text;
    // 페이로드 파싱: 대상 닉네임, 메시지 내용
    if (!proto::read_lp_utf8(sp, target) || !proto::read_lp_utf8(sp, text)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad whisper payload");
        return;
    }
    auto session_sp = s.shared_from_this();
    // whisper는 타 세션 조회와 저장소 접근이 섞이므로 job_queue로 넘긴다.
    // I/O 스레드에서 직접 처리하면 느린 조회 하나가 다른 클라이언트의 패킷 수신까지 막을 수 있다.
    if (!job_queue_.TryPush([this, session_sp, target = std::move(target), text = std::move(text)]() {
        dispatch_whisper(session_sp, target, text);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_whisper dropped: job queue full");
    }
}

// -----------------------------------------------------------------------------
// 일반 채팅 메시지 처리 핸들러
// -----------------------------------------------------------------------------
// 방에 있는 모든 사용자에게 메시지를 브로드캐스트합니다.
// 1. 권한 검사 (로그인 여부, 현재 방 일치 여부)
// 2. 슬래시 커맨드 처리 (/rooms, /who, /whisper 등)
// 3. 메시지 영속화 (DB 저장)
// 4. Redis Recent History 캐싱
// 5. 로컬 세션 브로드캐스트
// 6. Redis Pub/Sub을 통한 분산 브로드캐스트 (Fan-out)

void ChatService::on_chat_send(Session& s, std::span<const std::uint8_t> payload) {
    std::string room, text;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    // 페이로드 파싱: 방 이름, 메시지 내용
    if (!proto::read_lp_utf8(sp, room) || !proto::read_lp_utf8(sp, text)) { 
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad chat payload"); 
        return; 
    }

    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp, room = std::move(room), text = std::move(text)]() mutable {
        // /refresh는 상태 스냅샷을 강제로 다시 받게 하는 관리 명령이다.
        // 클라이언트 상태가 꼬였을 때 유용합니다.
        // 예: 네트워크 끊김 후 재접속 시 UI 갱신용
        if (text == "/refresh") {
            handle_refresh(session_sp);
            return;
        }

        // 현재 세션이 보고 있는 room 정보와 authoritative room을 비교한다.
        // 클라이언트가 주장하는 방과 서버가 알고 있는 방이 다르면 에러 처리합니다.
        std::string current_room = room;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            if (!impl_->state.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }
            auto it = impl_->state.cur_room.find(session_sp.get()); 
            if (it == impl_->state.cur_room.end()) { 
                session_sp->send_error(proto::errc::NO_ROOM, "no current room"); 
                return; 
            }
            const std::string& authoritative_room = it->second;
            if (!current_room.empty() && current_room != authoritative_room) {
                session_sp->send_error(proto::errc::ROOM_MISMATCH, "room mismatch");
                return;
            }
            current_room = authoritative_room;
        }

        const auto parse_duration_sec = [](const std::string& raw,
                                           std::uint32_t fallback,
                                           std::uint32_t min_value,
                                           std::uint32_t max_value) {
            if (raw.empty()) {
                return fallback;
            }
            try {
                const auto parsed = std::stoul(raw);
                if (parsed < min_value || parsed > max_value) {
                    return fallback;
                }
                return static_cast<std::uint32_t>(parsed);
            } catch (...) {
                return fallback;
            }
        };

        // 슬래시 명령 분기를 처리한다.
        // 채팅 메시지가 '/'로 시작하면 명령어로 간주합니다.
        if (!text.empty() && text[0] == '/') {
            if (text == "/rooms") { send_rooms_list(*session_sp); return; }
            if (text.rfind("/who", 0) == 0) {
                std::string target = current_room; 
                if (text.size() > 4) { 
                    auto pos = text.find_first_not_of(' ', 4); 
                    if (pos != std::string::npos) target = text.substr(pos); 
                }
                send_room_users(*session_sp, target); 
                return;
            }
            // 귓속말 단축 명령어 지원 (/w, /whisper)
            if (text.rfind("/whisper ", 0) == 0 || text.rfind("/w ", 0) == 0) {
                const bool long_form = text.rfind("/whisper ", 0) == 0;
                std::string args = text.substr(long_form ? 9 : 3);
                auto leading = args.find_first_not_of(' ');
                if (leading != std::string::npos && leading > 0) {
                    args.erase(0, leading);
                }
                auto spc = args.find(' ');
                if (spc == std::string::npos) {
                    send_system_notice(*session_sp, "usage: /whisper <user> <message>");
                    send_whisper_result(*session_sp, false, "invalid payload");
                    return;
                }
                std::string target_user = args.substr(0, spc);
                std::string wtext = args.substr(spc + 1);
                auto msg_leading = wtext.find_first_not_of(' ');
                if (msg_leading != std::string::npos && msg_leading > 0) {
                    wtext.erase(0, msg_leading);
                }
                if (target_user.empty() || wtext.empty()) {
                    send_system_notice(*session_sp, "usage: /whisper <user> <message>");
                    send_whisper_result(*session_sp, false, "invalid payload");
                    return;
                }
                dispatch_whisper(session_sp, target_user, wtext);
                return;
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
                    return;
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
                    return;
                }
                send_system_notice(*session_sp, "invited user: " + target_user + " room=" + target_room);
                return;
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
                    return;
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
                                    auto cur_room_it = impl_->state.cur_room.find(candidate.get());
                                    if (cur_room_it != impl_->state.cur_room.end() && cur_room_it->second == target_room) {
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
                    return;
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
                return;
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
                    return;
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
                    return;
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
                return;
            }

            if (text.rfind("/mute ", 0) == 0) {
                std::istringstream iss(text.substr(6));
                std::string target_user;
                std::string duration_raw;
                iss >> target_user;
                iss >> duration_raw;
                if (target_user.empty()) {
                    send_system_notice(*session_sp, "usage: /mute <user> [seconds]");
                    return;
                }
                const auto duration_sec = parse_duration_sec(duration_raw, impl_->runtime.spam_mute_sec, 5, 86400);

                bool allowed = false;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
                    if (allowed) {
                        impl_->state.muted_users[target_user] = {
                            std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec),
                            "muted by admin"
                        };
                    }
                }

                if (!allowed) {
                    send_system_notice(*session_sp, "mute denied: admin only");
                    return;
                }
                send_system_notice(*session_sp, "mute applied: user=" + target_user + " duration=" + std::to_string(duration_sec) + "s");
                return;
            }

            if (text.rfind("/unmute ", 0) == 0) {
                std::istringstream iss(text.substr(8));
                std::string target_user;
                iss >> target_user;
                if (target_user.empty()) {
                    send_system_notice(*session_sp, "usage: /unmute <user>");
                    return;
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
                    return;
                }
                send_system_notice(*session_sp, changed ? "unmute applied: user=" + target_user : "unmute no-op: user not muted");
                return;
            }

            if (text.rfind("/ban ", 0) == 0) {
                std::istringstream iss(text.substr(5));
                std::string target_user;
                std::string duration_raw;
                iss >> target_user;
                iss >> duration_raw;
                if (target_user.empty()) {
                    send_system_notice(*session_sp, "usage: /ban <user> [seconds]");
                    return;
                }
                const auto duration_sec = parse_duration_sec(duration_raw, impl_->runtime.spam_ban_sec, 10, 604800);

                bool allowed = false;
                std::vector<std::shared_ptr<Session>> banned_sessions;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
                    if (allowed) {
                        const auto expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
                        impl_->state.banned_users[target_user] = {expires_at, "banned by admin"};

                        if (auto ip_it = impl_->state.user_last_ip.find(target_user); ip_it != impl_->state.user_last_ip.end() && !ip_it->second.empty()) {
                            impl_->state.banned_ips[ip_it->second] = expires_at;
                        }
                        if (auto hwid_it = impl_->state.user_last_hwid_hash.find(target_user); hwid_it != impl_->state.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                            impl_->state.banned_hwid_hashes[hwid_it->second] = expires_at;
                        }

                        auto users_it = impl_->state.by_user.find(target_user);
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
                    return;
                }
                for (auto& banned : banned_sessions) {
                    send_system_notice(*banned, "temporarily banned");
                    banned->stop();
                }
                send_system_notice(*session_sp, "ban applied: user=" + target_user + " duration=" + std::to_string(duration_sec) + "s");
                return;
            }

            if (text.rfind("/unban ", 0) == 0) {
                std::istringstream iss(text.substr(7));
                std::string target_user;
                iss >> target_user;
                if (target_user.empty()) {
                    send_system_notice(*session_sp, "usage: /unban <user>");
                    return;
                }

                bool allowed = false;
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
                    if (allowed) {
                        changed = impl_->state.banned_users.erase(target_user) > 0;
                        if (auto ip_it = impl_->state.user_last_ip.find(target_user); ip_it != impl_->state.user_last_ip.end() && !ip_it->second.empty()) {
                            changed = impl_->state.banned_ips.erase(ip_it->second) > 0 || changed;
                        }
                        if (auto hwid_it = impl_->state.user_last_hwid_hash.find(target_user); hwid_it != impl_->state.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                            changed = impl_->state.banned_hwid_hashes.erase(hwid_it->second) > 0 || changed;
                        }
                    }
                }

                if (!allowed) {
                    send_system_notice(*session_sp, "unban denied: admin only");
                    return;
                }
                send_system_notice(*session_sp, changed ? "unban applied: user=" + target_user : "unban no-op: user not banned");
                return;
            }

            if (text.rfind("/gkick ", 0) == 0) {
                std::istringstream iss(text.substr(7));
                std::string target_user;
                iss >> target_user;
                if (target_user.empty()) {
                    send_system_notice(*session_sp, "usage: /gkick <user>");
                    return;
                }

                bool allowed = false;
                std::vector<std::shared_ptr<Session>> kicked_sessions;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    allowed = actor_is_admin_locked(impl_->state, impl_->runtime, *session_sp);
                    if (allowed) {
                        auto users_it = impl_->state.by_user.find(target_user);
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
                    return;
                }
                for (auto& kicked : kicked_sessions) {
                    send_system_notice(*kicked, "disconnected by administrator");
                    kicked->stop();
                }
                send_system_notice(*session_sp,
                                   kicked_sessions.empty() ? "global kick target not found" : "global kick applied");
                return;
            }

            if (text.rfind("/block ", 0) == 0 || text.rfind("/unblock ", 0) == 0 || text.rfind("/blacklist", 0) == 0) {
                std::string actor;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    actor = actor_name_locked(impl_->state, *session_sp);
                }
                if (actor.empty() || actor == "guest") {
                    send_system_notice(*session_sp, "blacklist denied: login required");
                    return;
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
                    return;
                }

                if (command.kind == BlacklistCommandKind::invalid || command.target_user.empty()) {
                    send_system_notice(*session_sp, "usage: /blacklist <add|remove|list> [user]");
                    return;
                }

                if (command.target_user == actor) {
                    send_system_notice(*session_sp, "blacklist denied: cannot target yourself");
                    return;
                }

                if (command.kind == BlacklistCommandKind::add) {
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lk(impl_->state.mu);
                        changed = add_blacklist_entry_locked(impl_->state, actor, command.target_user);
                    }
                    send_system_notice(*session_sp, changed ? "blacklist add: " + command.target_user : "blacklist add no-op");
                    return;
                }
                if (command.kind == BlacklistCommandKind::remove) {
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lk(impl_->state.mu);
                        changed = remove_blacklist_entry_locked(impl_->state, actor, command.target_user);
                    }
                    send_system_notice(*session_sp, changed ? "blacklist remove: " + command.target_user : "blacklist remove no-op");
                    return;
                }

                send_system_notice(*session_sp, "usage: /blacklist <add|remove|list> [user]");
                return;
            }
        }

        auto moderation = ChatSendModerationResult{};
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            moderation = evaluate_chat_send_moderation_locked(impl_->state, impl_->runtime, session_sp);
            if (moderation.sender == "guest") {
                session_sp->send_error(proto::errc::UNAUTHORIZED, "guest cannot chat");
                return;
            }
        }

        if (moderation.sender_is_banned) {
            send_system_notice(*session_sp, moderation.moderation_reason);
            session_sp->send_error(proto::errc::FORBIDDEN, moderation.moderation_reason);
            session_sp->stop();
            return;
        }
        if (moderation.sender_is_muted) {
            send_system_notice(*session_sp, moderation.moderation_reason);
            session_sp->send_error(proto::errc::FORBIDDEN, moderation.moderation_reason);
            return;
        }
        if (moderation.spam_escalated) {
            send_system_notice(*session_sp, moderation.moderation_reason);
            session_sp->send_error(proto::errc::FORBIDDEN, moderation.moderation_reason);
            if (moderation.spam_escalated_to_ban) {
                for (auto& penalized : moderation.penalized_sessions) {
                    send_system_notice(*penalized, moderation.moderation_reason);
                    penalized->stop();
                }
            }
            return;
        }

        // 플러그인은 내장 slash command 다음, 실제 fan-out 직전에 탄다.
        // 그래야 기본 명령 체계는 유지하면서도 마지막 정책 게이트로 moderation/변환을 덧댈 수 있다.
        if (maybe_handle_chat_hook_plugin(*session_sp, current_room, moderation.sender, text)) {
            return;
        }

        // 일반 채널 브로드캐스트 경로.
        std::vector<std::shared_ptr<Session>> targets;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            auto it = impl_->state.rooms.find(current_room);
            if (it != impl_->state.rooms.end()) {
                collect_room_sessions(it->second, targets);
            }

            std::vector<std::shared_ptr<Session>> filtered_targets;
            filtered_targets.reserve(targets.size());
            for (auto& target : targets) {
                auto receiver_it = impl_->state.user.find(target.get());
                if (receiver_it == impl_->state.user.end()) {
                    continue;
                }
                const std::string& receiver = receiver_it->second;
                if (auto blk_it = impl_->state.user_blacklists.find(receiver);
                    blk_it != impl_->state.user_blacklists.end() && blk_it->second.count(moderation.sender) > 0) {
                    continue;
                }
                filtered_targets.push_back(target);
            }
            targets = std::move(filtered_targets);
        }

        // Protobuf 메시지 생성
        server::wire::v1::ChatBroadcast pb; 
        pb.set_room(current_room); 
        pb.set_sender(moderation.sender); 
        pb.set_text(text); 
        pb.set_sender_sid(session_sp->session_id());
        auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
        pb.set_ts_ms(static_cast<std::uint64_t>(now64));
        std::string bytes; pb.SerializeToString(&bytes);

        // 영속 저장소는 recent-history 캐시가 비거나 Redis가 재시작돼도 대화 이력을 복구하는 최후 근거다.
        // 캐시만 믿으면 빠르지만, 장애 후에는 방 히스토리가 통째로 사라질 수 있다.
        std::string persisted_room_id;
        std::uint64_t persisted_msg_id = 0;
        if (impl_->runtime.db_pool) {
            try {
                persisted_room_id = ensure_room_id_ci(current_room);
                if (!persisted_room_id.empty()) {
                    std::optional<std::string> uid_opt;
                    {
                        std::lock_guard<std::mutex> lk(impl_->state.mu);
                        auto it = impl_->state.user_uuid.find(session_sp.get());
                        if (it != impl_->state.user_uuid.end()) uid_opt = it->second;
                    }
                    auto uow = impl_->runtime.db_pool->make_repository_unit_of_work();
                    auto msg = uow->messages().create(persisted_room_id, current_room, uid_opt, text);
                    persisted_msg_id = msg.id;
                    uow->commit();
                }
            } catch (const std::exception& e) {
                corelog::error(std::string("Failed to persist message: ") + e.what());
            }
        }

        // Redis recent-history는 DB가 정한 메시지 ID를 그대로 따라간다.
        // 캐시가 별도 번호 체계를 쓰면 refresh가 캐시와 DB 결과를 합칠 때 중복 제거가 어려워진다.
        if (impl_->runtime.redis && !persisted_room_id.empty() && persisted_msg_id != 0) {
            server::wire::v1::StateSnapshot::SnapshotMessage snapshot_msg;
            snapshot_msg.set_id(persisted_msg_id);
            snapshot_msg.set_sender(moderation.sender);
            snapshot_msg.set_text(text);
            snapshot_msg.set_ts_ms(static_cast<std::uint64_t>(now64));
            if (!ChatServicePrivateAccess::cache_recent_message(*this, persisted_room_id, snapshot_msg)) {
                corelog::warn(std::string("Redis recent history update failed for room_id=") + persisted_room_id);
            } else {
                // Debug log
                // corelog::info("Cached message " + std::to_string(persisted_msg_id) + " to room " + persisted_room_id);
            }
        } else {
            if (!impl_->runtime.redis) corelog::warn("Redis not available for caching");
            if (persisted_room_id.empty()) corelog::warn("Room ID not found for caching");
            if (persisted_msg_id == 0) corelog::warn("Message ID not generated (DB persist failed?)");
        }

        // 로컬 세션들에게 메시지 전송
        std::vector<std::uint8_t> broadcast_body(bytes.begin(), bytes.end());
        if (targets.empty()) {
            // 방에 혼자 있는 경우 자신에게만 에코
            session_sp->async_send(game_proto::MSG_CHAT_BROADCAST, broadcast_body, proto::FLAG_SELF);
        } else {
            for (auto& t : targets) {
                auto f = (t.get() == session_sp.get()) ? proto::FLAG_SELF : 0;
                t->async_send(game_proto::MSG_CHAT_BROADCAST, broadcast_body, f);
            }
        }

        // 사용자 프레즌스 heartbeat TTL을 갱신한다.
        // 채팅 활동이 있으면 온라인 상태로 간주하여 TTL을 연장합니다.
        if (impl_->runtime.redis) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    auto it = impl_->state.user_uuid.find(session_sp.get());
                    if (it != impl_->state.user_uuid.end()) uid = it->second;
                }
                touch_user_presence(uid);
            } catch (...) {}
        }

        // 분산 fan-out은 로컬 fan-out 이후에 수행한다.
        // 로컬 사용자에게는 가장 짧은 경로로 먼저 보내고, 다른 서버 확산은 별도 레이어에 맡기는 편이 지연과 장애 분석에 유리하다.
        if (impl_->runtime.redis && pubsub_enabled()) {
            try {
                static std::atomic<std::uint64_t> publish_total{0};
                std::string channel = impl_->presence.prefix + std::string("fanout:room:") + current_room;
                std::string message;
                message.reserve(3 + impl_->runtime.gateway_id.size() + bytes.size());
                message.append("gw=").append(impl_->runtime.gateway_id);
                message.push_back('\n');
                message.append(bytes);
                impl_->runtime.redis->publish(channel, std::move(message));
                auto n = ++publish_total;
                if ((n & 1023ULL) == 0) {
                    // 핫패스 로그 부하를 낮추기 위해 1024건마다 샘플링해서 남긴다.
                    corelog::debug(std::string("metric=publish_total value=") + std::to_string(n) + " room=" + current_room);
                }
            } catch (...) {}
        }

        // 마지막으로 본 메시지 id를 membership.last_seen에 반영한다.
        // 사용자가 어디까지 읽었는지 추적하는 기능입니다 (Read Receipt).
        if (impl_->runtime.db_pool && persisted_msg_id > 0 && !persisted_room_id.empty()) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    auto it = impl_->state.user_uuid.find(session_sp.get());
                    if (it != impl_->state.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    auto uow = impl_->runtime.db_pool->make_repository_unit_of_work();
                    uow->memberships().update_last_seen(uid, persisted_room_id, persisted_msg_id);
                    uow->commit();
                }
            } catch (...) {}
        }
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_chat_send dropped: job queue full");
    }
}

// -----------------------------------------------------------------------------
// 상태 갱신 (Refresh) 핸들러
// -----------------------------------------------------------------------------
// 클라이언트가 현재 상태(방 정보, 최근 메시지 등)를 다시 요청할 때 사용합니다.
// 주로 네트워크 재연결 후 상태 동기화를 위해 호출됩니다.

void ChatService::handle_refresh(std::shared_ptr<Session> session_sp) {
    std::string current;
    {
        std::lock_guard<std::mutex> lk(impl_->state.mu);
        auto itcr = impl_->state.cur_room.find(session_sp.get());
        current = (itcr != impl_->state.cur_room.end()) ? itcr->second : std::string("lobby");
    }

    // 현재 방의 상태 스냅샷 전송
    send_snapshot(*session_sp, current);

    if (!impl_->runtime.db_pool) {
        return;
    }
    // DB에 마지막 읽은 위치 갱신 (현재 방의 최신 메시지로)
    try {
        std::string uid;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            auto it = impl_->state.user_uuid.find(session_sp.get());
            if (it != impl_->state.user_uuid.end()) uid = it->second;
        }
        if (uid.empty()) return;
        auto rid = ensure_room_id_ci(current);
        if (rid.empty()) return;
        auto uow = impl_->runtime.db_pool->make_repository_unit_of_work();
        auto last_id = uow->messages().get_last_id(rid);
        if (last_id > 0) {
            uow->memberships().update_last_seen(uid, rid, last_id);
            uow->commit();
        }
    } catch (...) {
    }
}

void ChatService::on_rooms_request(Session& s, std::span<const std::uint8_t>) {
    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp]() {
        send_rooms_list(*session_sp);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_rooms_request dropped: job queue full");
    }
}

void ChatService::on_room_users_request(Session& s, std::span<const std::uint8_t> payload) {
    std::string requested;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    proto::read_lp_utf8(sp, requested);
    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp, requested = std::move(requested)]() mutable {
        std::string target = requested;
        if (target.empty()) {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            auto it = impl_->state.cur_room.find(session_sp.get());
            target = (it != impl_->state.cur_room.end()) ? it->second : std::string("lobby");
        }
        send_room_users(*session_sp, target);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_room_users_request dropped: job queue full");
    }
}

void ChatService::on_refresh_request(Session& s, std::span<const std::uint8_t>) {
    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp]() {
        handle_refresh(session_sp);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_refresh_request dropped: job queue full");
    }
}

} // namespace server::app::chat

