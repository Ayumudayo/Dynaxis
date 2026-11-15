#include "server/chat/chat_service.hpp"
#include "chat_service_state.hpp"
#include "chat_room_state.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
#include "server/storage/connection_pool.hpp"
#include "wire.pb.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <nlohmann/json.hpp>

namespace server::app::chat {
namespace {
using Json = nlohmann::json;
namespace proto = server::core::protocol;
constexpr std::uint64_t login_lease_ms = 120000;
std::uint32_t parse_duration_sec(const std::string& value, std::uint32_t fallback, std::uint32_t minimum, std::uint32_t maximum) {
    if (value.empty()) return fallback;
    try { const auto seconds = std::stoul(value); return seconds >= minimum && seconds <= maximum ? static_cast<std::uint32_t>(seconds) : fallback; }
    catch (...) { return fallback; }
}
Json read_state(server::storage::IChatStateRepository& repository, const std::string& key) {
    const auto value = repository.get(key);
    return value ? Json::parse(*value) : Json::object();
}
std::string user_of(ChatServiceState& state, ChatService::NetSession& session) {
    const auto it = state.user.find(&session);
    return it == state.user.end() ? std::string{} : it->second;
}
}

bool ChatService::enqueue_session(std::shared_ptr<NetSession> session, std::function<void()> job, bool closing) {
    auto& dispatch = impl_->dispatch;
    auto runner = [this, session] {
        for (;;) {
            std::function<void()> next;
            {
                std::lock_guard lock(impl_->dispatch.mu);
                auto it = impl_->dispatch.sessions.find(session.get());
                if (it == impl_->dispatch.sessions.end()) return;
                if (it->second.empty()) { impl_->dispatch.sessions.erase(it); return; }
                next = std::move(it->second.front());
                it->second.pop_front();
                --impl_->dispatch.pending;
            }
            try { next(); }
            catch (...) {
                // DB/권한 조회 실패를 허용으로 바꾸지 않는다. 자격/본문은 기록하지 않는다.
                session->send_error(proto::errc::SERVER_BUSY, "shared state temporarily unavailable");
                server::core::log::warn("chat session operation failed");
            }
        }
    };
    std::unique_lock lock(dispatch.mu);
    auto it = dispatch.sessions.find(session.get());
    const auto budget = job_queue_.max_size() ? job_queue_.max_size() : 4096;
    if (!closing && (dispatch.pending >= budget || (it != dispatch.sessions.end() && it->second.size() >= 128))) return false;
    const bool first = it == dispatch.sessions.end();
    dispatch.sessions[session.get()].push_back([this, session, job = std::move(job), closing] {
        if (closing || renew_shared_login(*session)) job();
    });
    ++dispatch.pending;
    if (!first) return true;
    if (job_queue_.TryPush(runner)) return true;
    if (closing) {
        lock.unlock();
        runner(); // close만 있고 앞선 작업이 없는 세션: 큐 포화여도 정리는 유실하지 않는다.
        return true;
    }
    dispatch.sessions.erase(session.get());
    --dispatch.pending;
    return false;
}

bool ChatService::claim_shared_login(NetSession& session, const std::string& user, const std::string& resume_logical_id) {
    if (!impl_->runtime.db_pool) return true;
    const auto token = get_or_create_session_uuid(session);
    auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
    auto& repository = work->chat_state();
    const std::string key = "login:" + user;
    repository.lock(key);
    auto lease = read_state(repository, key);
    const auto now = repository.now_ms();
    if (lease.value("expires", 0ULL) > now && lease.value("token", "") != token
        && (resume_logical_id.empty() || lease.value("logical", "") != resume_logical_id)) {
        session.send_error(proto::errc::NAME_TAKEN, "name taken");
        return false;
    }
    const auto penalty = read_state(repository, "penalty:" + user);
    if (penalty.value("ban", 0ULL) > now) {
        session.send_error(proto::errc::FORBIDDEN, "temporarily banned");
        return false;
    }
    const bool replaced = !lease.empty() && lease.value("token", "") != token;
    repository.put(key, Json{{"token", token}, {"logical", resume_logical_id}, {"expires", now + login_lease_ms}}.dump());
    work->commit();
    if (replaced) publish_control_change();
    return true;
}

void ChatService::deliver_verified(std::shared_ptr<NetSession> target, std::uint16_t opcode,
                                  std::vector<std::uint8_t> body, std::string sender, std::string room) {
    enqueue_session(target, [this, target, opcode, body = std::move(body), sender = std::move(sender), room = std::move(room)] {
        std::lock_guard lock(impl_->state.mu);
        if (!impl_->state.authed.count(target.get())) return;
        const auto user = user_of(impl_->state, *target);
        if (impl_->state.user_blacklists[user].count(sender)) return;
        if (!room.empty()) {
            if (impl_->state.cur_room[target.get()] != room) return;
            server::wire::v1::ChatBroadcast broadcast;
            if (!broadcast.ParseFromArray(body.data(), static_cast<int>(body.size()))) return;
            if (!broadcast.room_id().empty() && impl_->state.room_ids[room] != broadcast.room_id()) return;
        }
        target->async_send(opcode, body, 0);
    });
}

bool ChatService::renew_shared_login(NetSession& session, const std::string& logical_id) {
    std::string user, room;
    {
        std::lock_guard lock(impl_->state.mu);
        if (!impl_->state.authed.count(&session)) return true;
        user = user_of(impl_->state, session);
        room = impl_->state.cur_room[&session];
    }
    if (!impl_->runtime.db_pool) return true;
    const auto token = get_or_create_session_uuid(session);
    auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
    auto& repository = work->chat_state();
    const auto key = "login:" + user;
    repository.lock(key);
    auto lease = read_state(repository, key);
    if (lease.value("token", "") != token) {
        session.send_error(proto::errc::UNAUTHORIZED, "session replaced or expired");
        session.stop();
        return false;
    }
    const auto now = repository.now_ms();
    const auto penalty = read_state(repository, "penalty:" + user);
    if (penalty.value("ban", 0ULL) > now) {
        session.send_error(proto::errc::FORBIDDEN, "temporarily banned");
        session.stop();
        return false;
    }
    lease["expires"] = now + login_lease_ms;
    if (!logical_id.empty()) lease["logical"] = logical_id;
    repository.put(key, lease.dump());
    bool removed = false;
    if (room != "lobby" && !room.empty()) {
        const auto authority = read_state(repository, "room:" + room);
        removed = !authority.value("active", false) || !authority.value("members", Json::object()).value(user, false);
    }
    const auto blocked = read_state(repository, "blacklist:" + user);
    work->commit();
    {
        std::lock_guard lock(impl_->state.mu);
        const auto mute = penalty.value("mute", 0ULL);
        impl_->state.banned_users.erase(user); // DB의 ban 해제/만료를 로컬 과거 제재가 덮어쓰지 않는다.
        if (mute > now) impl_->state.muted_users[user] = {std::chrono::steady_clock::now() + std::chrono::milliseconds(mute - now), "muted"};
        else impl_->state.muted_users.erase(user);
        auto& entries = impl_->state.user_blacklists[user];
        entries.clear();
        for (const auto& entry : blocked.value("users", Json::array())) entries.insert(entry.get<std::string>());
        if (removed) {
            impl_->state.room_ids.erase(room);
            remove_session_from_room_locked(impl_->state, session.shared_from_this(), room, user);
            place_session_in_room_locked(impl_->state, session.shared_from_this(), "lobby", user);
        }
    }
    if (removed) {
        persist_continuity_room([&] { std::lock_guard lock(impl_->state.mu); return impl_->state.logical_session_id[&session]; }(), "lobby",
            [&] { std::lock_guard lock(impl_->state.mu); return impl_->state.logical_session_expires_unix_ms[&session]; }());
        if (impl_->runtime.redis) {
            impl_->runtime.redis->srem("room:users:" + room, user);
            impl_->runtime.redis->sadd("room:users:lobby", user);
        }
        broadcast_refresh(room);
        broadcast_refresh("lobby");
        send_system_notice(session, "room access revoked");
        send_snapshot(session, "lobby");
    }
    return true;
}

bool ChatService::release_shared_login(NetSession& session) {
    std::string user, uid, room, room_id;
    { std::lock_guard lock(impl_->state.mu);
      user = user_of(impl_->state, session);
      if (auto it = impl_->state.user_uuid.find(&session); it != impl_->state.user_uuid.end()) uid = it->second;
      if (auto it = impl_->state.cur_room.find(&session); it != impl_->state.cur_room.end()) room = it->second;
      if (auto it = impl_->state.room_ids.find(room); it != impl_->state.room_ids.end()) room_id = it->second;
    }
    if (!impl_->runtime.db_pool || user.empty()) return true;
    try {
        const auto token = get_or_create_session_uuid(session);
        auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
        auto& repository = work->chat_state();
        const auto key = "login:" + user;
        repository.lock(key);
        if (read_state(repository, key).value("token", "") != token) return false;
        // 새 로그인/resume의 예약 변경과 배타적으로 정리한다. commit 뒤 옛 연결이
        // Redis를 지우면 새 소유자의 presence까지 제거하는 경쟁이 생긴다.
        if (impl_->runtime.redis) {
            if (!room.empty()) impl_->runtime.redis->srem("room:users:" + room, user);
            if (!uid.empty()) {
                impl_->runtime.redis->del(make_presence_key("presence:user:", uid));
                if (!room_id.empty()) impl_->runtime.redis->srem(make_presence_key("presence:room:", room_id), uid);
            }
        }
        repository.erase(key);
        work->commit();
        return true;
    } catch (...) { return false; }
}

bool ChatService::admit_shared_room(NetSession& session, std::string& room, const std::string& password, bool resume) {
    if (room == "lobby") return true;
    std::string user;
    { std::lock_guard lock(impl_->state.mu); user = user_of(impl_->state, session); }
    if (user.empty()) { session.send_error(proto::errc::UNAUTHORIZED, "unauthorized"); return false; }
    if (!impl_->runtime.db_pool) {
        session.send_error(proto::errc::SERVER_BUSY, "room authority unavailable");
        return false;
    }
    auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
    auto& repository = work->chat_state();
    const auto key = "room:" + room;
    repository.lock(key);
    auto authority = read_state(repository, key);
    const bool fresh = authority.empty() || !authority.value("active", true);
    if (fresh && resume) return false;
    if (fresh) authority = {{"owner", resume ? "" : user}, {"password", password.empty() ? "" : hash_room_password(password)},
                             {"invites", Json::array()}, {"members", Json::object()}, {"active", true}};
    const auto owner = authority.value("owner", "");
    auto invites = authority.value("invites", Json::array());
    auto members = authority.value("members", Json::object());
    const bool invited = std::find(invites.begin(), invites.end(), Json(user)) != invites.end();
    const auto expected = authority.value("password", "");
    if (!fresh && owner != user && !impl_->runtime.admin_users.count(user) && !invited
        && !(resume && members.value(user, false))
        && (password.empty() || expected.empty() || !verify_room_password(password, expected))) {
        session.send_error(proto::errc::FORBIDDEN, expected.empty() ? "invite required" : "room locked");
        return false;
    }
    invites.erase(std::remove(invites.begin(), invites.end(), Json(user)), invites.end());
    authority["invites"] = invites;
    members[user] = true;
    authority["members"] = members;
    repository.put(key, authority.dump());
    auto stored_room = work->rooms().find_by_name_exact_ci(room);
    if (!stored_room) stored_room = work->rooms().create(room, expected.empty());
    room = stored_room->name;
    work->commit();
    { std::lock_guard lock(impl_->state.mu);
      impl_->state.room_owners[room] = owner;
      impl_->state.room_ids[room] = stored_room->id;
      if (!expected.empty()) impl_->state.room_passwords[room] = expected;
    }
    return true;
}

void ChatService::publish_control_change() {
    if (impl_->runtime.redis) (void)impl_->runtime.redis->publish(impl_->presence.prefix + "fanout:control", "gw=" + impl_->runtime.gateway_id + "\nchanged");
    enforce_shared_state();
}

void ChatService::enforce_shared_state() {
    std::vector<std::shared_ptr<NetSession>> sessions;
    { std::lock_guard lock(impl_->state.mu);
      for (const auto& [_, weak] : impl_->state.by_session_id) if (auto session = weak.lock()) sessions.push_back(std::move(session));
    }
    for (auto& session : sessions) enqueue_session(session, [] {});
}

void ChatService::persist_shared_penalty(const std::string& user, bool ban, std::uint32_t seconds) {
    if (!impl_->runtime.db_pool) return;
    auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
    auto& repository = work->chat_state();
    const auto key = "penalty:" + user;
    repository.lock(key);
    auto penalty = read_state(repository, key);
    penalty[ban ? "ban" : "mute"] = seconds ? repository.now_ms() + seconds * 1000ULL : 0ULL;
    repository.put(key, penalty.dump());
    work->commit();
    publish_control_change();
}

void ChatService::persist_shared_blacklist(const std::string& user) {
    if (!impl_->runtime.db_pool) return;
    Json users = Json::array();
    { std::lock_guard lock(impl_->state.mu); for (const auto& entry : impl_->state.user_blacklists[user]) users.push_back(entry); }
    auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
    work->chat_state().lock("blacklist:" + user);
    work->chat_state().put("blacklist:" + user, Json{{"users", users}}.dump());
    work->commit();
}

bool ChatService::handle_shared_room_command(std::shared_ptr<NetSession> session, const std::string& current, const std::string& text) {
    std::istringstream input(text);
    std::string command, target, room;
    input >> command >> target >> room;
    const bool invite = command == "/invite", kick = command == "/kick", remove = command == "/room" && target == "remove";
    const bool ban = command == "/ban" || command == "/unban", mute = command == "/mute" || command == "/unmute", global_kick = command == "/gkick";
    if (!invite && !kick && !remove && !ban && !mute && !global_kick) return false;
    std::string actor;
    { std::lock_guard lock(impl_->state.mu); actor = user_of(impl_->state, *session); }
    const bool admin = impl_->runtime.admin_users.count(actor) > 0;
    if (!impl_->runtime.db_pool) { send_system_notice(*session, "shared authority unavailable"); return true; }
    if (ban || mute || global_kick) {
        if (!admin) { send_system_notice(*session, global_kick ? "global kick denied: admin only" : command.substr(1) + " denied: admin only"); return true; }
        if (target.empty()) { send_system_notice(*session, "target required"); return true; }
        if (global_kick) {
            auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
            work->chat_state().lock("login:" + target);
            work->chat_state().erase("login:" + target);
            work->commit();
            publish_control_change();
        } else {
            const bool undo = command == "/unban" || command == "/unmute";
            const auto seconds = undo ? 0u : parse_duration_sec(room, ban ? impl_->runtime.spam_ban_sec : impl_->runtime.spam_mute_sec, ban ? 10 : 5, ban ? 604800 : 86400);
            persist_shared_penalty(target, ban, seconds);
        }
        send_system_notice(*session, command.substr(1) + " applied: user=" + target);
        return true;
    }
    if (room.empty()) room = current;
    if (room == "lobby" || room.empty() || (!remove && target.empty())) { send_system_notice(*session, "invalid room command"); return true; }
    auto work = impl_->runtime.db_pool->make_repository_unit_of_work();
    auto& repository = work->chat_state();
    const auto key = "room:" + room;
    repository.lock(key);
    auto authority = read_state(repository, key);
    if (!authority.value("active", false) || (!admin && authority.value("owner", "") != actor)) {
        send_system_notice(*session, "room command denied: owner/admin only");
        return true;
    }
    if (invite) {
        auto invites = authority.value("invites", Json::array());
        if (std::find(invites.begin(), invites.end(), Json(target)) == invites.end()) invites.push_back(target);
        authority["invites"] = invites;
    } else if (kick) {
        auto members = authority.value("members", Json::object());
        members[target] = false;
        authority["members"] = members;
    } else {
        authority["active"] = false;
        if (auto existing = work->rooms().find_by_name_exact_ci(room)) work->rooms().close(existing->id);
    }
    repository.put(key, authority.dump());
    work->commit();
    if (remove) {
        std::lock_guard lock(impl_->state.mu);
        impl_->state.room_ids.erase(room);
        impl_->state.room_owners.erase(room);
        impl_->state.room_invites.erase(room);
    }
    if (remove && impl_->runtime.redis) impl_->runtime.redis->srem("rooms:active", room);
    publish_control_change();
    send_system_notice(*session, invite ? "invited user: " + target + " room=" + room : (remove ? "room removed: " + room : "kick applied"));
    return true;
}
} // namespace server::app::chat
