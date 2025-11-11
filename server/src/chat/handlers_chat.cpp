#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// 저장소 연동 헤더
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"
#include "server/storage/redis/client.hpp"
#include <vector>
#include <cstdlib>
#include <atomic>

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {


void ChatService::on_whisper(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string target;
    std::string text;
    if (!proto::read_lp_utf8(sp, target) || !proto::read_lp_utf8(sp, text)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad whisper payload");
        return;
    }
    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, target = std::move(target), text = std::move(text)]() {
        dispatch_whisper(session_sp, target, text);
    });
}

void ChatService::on_chat_send(Session& s, std::span<const std::uint8_t> payload) {
    std::string room, text;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    if (!proto::read_lp_utf8(sp, room) || !proto::read_lp_utf8(sp, text)) { 
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad chat payload"); 
        return; 
    }

    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, room, text]() {
        corelog::info(std::string("CHAT_SEND: room=") + (room.empty()?"(empty)":room) + ", text=" + text);
        // 슬래시 명령으로 들어온 /refresh는 서버에서 즉시 스냅샷을 전송해 준다.
        if (text == "/refresh") {
            handle_refresh(session_sp);
            return;
        }
        // 인증 상태와 현재 방 정보를 확인한다.
        std::string current_room = room;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }
            auto it = state_.cur_room.find(session_sp.get()); 
            if (it == state_.cur_room.end()) { 
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
        // 슬래시 명령 분기를 처리한다.
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
        }
        // 일반 채널 브로드캐스트 경로.
        std::vector<std::shared_ptr<Session>> targets;
        std::string sender;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it2 = state_.user.find(session_sp.get()); 
            sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
            if (sender == "guest") { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "guest cannot chat"); 
                return; 
            }
            auto it = state_.rooms.find(current_room); 
            if (it != state_.rooms.end()) { 
                collect_room_sessions(it->second, targets);
            }
        }
        server::wire::v1::ChatBroadcast pb; 
        pb.set_room(current_room); 
        pb.set_sender(sender); 
        pb.set_text(text); 
        pb.set_sender_sid(session_sp->session_id());
        auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
        pb.set_ts_ms(static_cast<std::uint64_t>(now64));
        std::string bytes; pb.SerializeToString(&bytes);
        // 영속화(옵션): Postgres 메시지 저장 후 Redis 최근 목록을 갱신한다.
        std::string persisted_room_id;
        std::uint64_t persisted_msg_id = 0;
        if (db_pool_) {
            try {
                persisted_room_id = ensure_room_id_ci(current_room);
                if (!persisted_room_id.empty()) {
                    std::optional<std::string> uid_opt;
                    {
                        std::lock_guard<std::mutex> lk(state_.mu);
                        auto it = state_.user_uuid.find(session_sp.get());
                        if (it != state_.user_uuid.end()) uid_opt = it->second;
                    }
                    auto uow = db_pool_->make_unit_of_work();
                    auto msg = uow->messages().create(persisted_room_id, current_room, uid_opt, text);
                    persisted_msg_id = msg.id;
                    uow->commit();
                }
            } catch (const std::exception& e) {
                corelog::error(std::string("Failed to persist message: ") + e.what());
            }
        }
        if (redis_ && !persisted_room_id.empty() && persisted_msg_id != 0) {
            server::wire::v1::StateSnapshot::SnapshotMessage snapshot_msg;
            snapshot_msg.set_id(persisted_msg_id);
            snapshot_msg.set_sender(sender);
            snapshot_msg.set_text(text);
            snapshot_msg.set_ts_ms(static_cast<std::uint64_t>(now64));
            if (!cache_recent_message(persisted_room_id, snapshot_msg)) {
                corelog::warn(std::string("Redis recent history update failed for room_id=") + persisted_room_id);
            }
        }
        if (targets.empty()) { 
            session_sp->async_send(proto::MSG_CHAT_BROADCAST, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), proto::FLAG_SELF); 
        } else { 
            for (auto& t : targets) { 
                auto f = (t.get() == session_sp.get()) ? proto::FLAG_SELF : 0; 
                t->async_send(proto::MSG_CHAT_BROADCAST, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), f); 
            } 
        }
        // 사용자 프레즌스 heartbeat TTL을 갱신한다.
        if (redis_) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                touch_user_presence(uid);
            } catch (...) {}
        }
        // Redis Pub/Sub 팬아웃(옵션)을 수행한다.
        if (redis_) {
            try {
                if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
                    static std::atomic<std::uint64_t> publish_total{0};
                    std::string channel = presence_.prefix + std::string("fanout:room:") + current_room;
                    std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    std::string message;
                    message.reserve(3 + gateway_id_.size() + payload.size());
                    message.append("gw=").append(gateway_id_);
                    message.push_back('\n');
                    message.append(payload);
                    redis_->publish(channel, std::move(message));
                    auto n = ++publish_total;
                    corelog::info(std::string("metric=publish_total value=") + std::to_string(n) + " room=" + current_room);
                }
            } catch (...) {}
        }
        // 마지막으로 본 메시지 id를 membership.last_seen에 반영한다.
        if (db_pool_ && persisted_msg_id > 0 && !persisted_room_id.empty()) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    auto uow = db_pool_->make_unit_of_work();
                    uow->memberships().update_last_seen(uid, persisted_room_id, persisted_msg_id);
                    uow->commit();
                }
            } catch (...) {}
        }
    });
}

void ChatService::handle_refresh(std::shared_ptr<Session> session_sp) {
    std::string current;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itcr = state_.cur_room.find(session_sp.get());
        current = (itcr != state_.cur_room.end()) ? itcr->second : std::string("lobby");
    }
    send_snapshot(*session_sp, current);

    if (!db_pool_) {
        return;
    }
    try {
        std::string uid;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it = state_.user_uuid.find(session_sp.get());
            if (it != state_.user_uuid.end()) uid = it->second;
        }
        if (uid.empty()) return;
        auto rid = ensure_room_id_ci(current);
        if (rid.empty()) return;
        auto uow = db_pool_->make_unit_of_work();
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
    job_queue_.Push([this, session_sp]() {
        send_rooms_list(*session_sp);
    });
}

void ChatService::on_room_users_request(Session& s, std::span<const std::uint8_t> payload) {
    std::string requested;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    proto::read_lp_utf8(sp, requested);
    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, requested = std::move(requested)]() mutable {
        std::string target = requested;
        if (target.empty()) {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it = state_.cur_room.find(session_sp.get());
            target = (it != state_.cur_room.end()) ? it->second : std::string("lobby");
        }
        send_room_users(*session_sp, target);
    });
}

void ChatService::on_refresh_request(Session& s, std::span<const std::uint8_t>) {
    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp]() {
        handle_refresh(session_sp);
    });
}

} // namespace server::app::chat


