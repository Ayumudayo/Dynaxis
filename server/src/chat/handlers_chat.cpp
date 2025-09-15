// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// storage
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"
#include "server/storage/redis/client.hpp"\n#include <vector>
#include <cstdlib>

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

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
        // /refresh는 인증 없이도 허용(스냅샷 반환)
        if (text == "/refresh") {
            std::string current;
            {
                std::lock_guard<std::mutex> lk(state_.mu); 
                auto itcr = state_.cur_room.find(session_sp.get()); 
                current = (itcr != state_.cur_room.end()) ? itcr->second : std::string("lobby");
            }
            send_snapshot(*session_sp, current); 
            // 읽음 처리: 현재 방의 마지막 메시지 id로 last_seen 갱신
            if (db_pool_) {
                try {
                    std::string uid;
                    {
                        std::lock_guard<std::mutex> lk(state_.mu);
                        auto it = state_.user_uuid.find(session_sp.get());
                        if (it != state_.user_uuid.end()) uid = it->second;
                    }
                    if (!uid.empty()) {
                        auto rid = ensure_room_id_ci(current);
                        if (!rid.empty()) {
                            auto uow = db_pool_->make_unit_of_work();
                            auto last_id = uow->messages().get_last_id(rid);
                            if (last_id > 0) {
                                uow->memberships().update_last_seen(uid, rid, last_id);
                                uow->commit();
                            }
                        }
                    }
                } catch (...) {}
            }
            return;
        }
        // 인증 체크
        std::string current_room = room;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }
            if (current_room.empty()) {
                auto it = state_.cur_room.find(session_sp.get()); 
                if (it == state_.cur_room.end()) { 
                    session_sp->send_error(proto::errc::NO_ROOM, "no current room"); 
                    return; 
                } 
                current_room = it->second;
            }
        }
        // 슬래시 명령
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
            if (text.rfind("/whisper ", 0) == 0) {
                std::string rest = text.substr(9); auto spc = rest.find(' ');
                if (spc == std::string::npos) {
                    std::vector<std::uint8_t> body; 
                    proto::write_lp_utf8(body, std::string("(system)")); 
                    proto::write_lp_utf8(body, std::string("server")); 
                    proto::write_lp_utf8(body, std::string("usage: /whisper <user> <message>"));
                    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
                    std::size_t off = body.size(); 
                    body.resize(off + 8); 
                    std::uint64_t ts = static_cast<std::uint64_t>(now64); 
                    for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; } 
                    session_sp->async_send(proto::MSG_CHAT_BROADCAST, body, 0); 
                    return;
                }
                std::string target_user = rest.substr(0, spc); 
                std::string wtext = rest.substr(spc + 1);
                std::vector<std::shared_ptr<Session>> targets;
                {
                    std::lock_guard<std::mutex> lk(state_.mu); 
                    auto itset = state_.by_user.find(target_user); 
                    if (itset != state_.by_user.end()) { 
                        for (auto wit = itset->second.begin(); wit != itset->second.end(); ) { 
                            if (auto p = wit->lock()) { 
                                targets.emplace_back(std::move(p)); 
                                ++wit; 
                            } else { 
                                wit = itset->second.erase(wit); 
                            } 
                        } 
                    }
                }
                std::string sender; 
                { 
                    std::lock_guard<std::mutex> lk(state_.mu); 
                    auto it2 = state_.user.find(session_sp.get()); 
                    sender = (it2 != state_.user.end()) ? it2->second : std::string("guest"); 
                }
                server::wire::v1::ChatBroadcast pb; 
                pb.set_room("(direct)"); 
                pb.set_sender(sender); 
                pb.set_text(std::string("(to ") + target_user + ") " + wtext); 
                pb.set_sender_sid(session_sp->session_id());
                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
                pb.set_ts_ms(static_cast<std::uint64_t>(now64));
                std::string bytes; pb.SerializeToString(&bytes);
                std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
                targets.emplace_back(session_sp); 
                for (auto& t : targets) t->async_send(proto::MSG_CHAT_BROADCAST, body, 0); 
                return;
            }
        }
        // 일반 채팅
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
                auto& set = it->second; 
                for (auto wit = set.begin(); wit != set.end(); ) { 
                    if (auto p = wit->lock()) { 
                        targets.emplace_back(std::move(p)); 
                        ++wit; 
                    } else { 
                        wit = set.erase(wit); 
                    } 
                } 
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
        // 영속화(최소 경로): Postgres 메시지 저장 + Redis 최근 리스트 추가
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
                corelog::error(std::string("메시지 영속화 실패: ") + e.what());
            }
        }
        if (redis_ && !persisted_room_id.empty() && persisted_msg_id != 0) {
            // 매우 단순한 JSON 문자열 구성(스냅샷 포맷과 별개, 스모크용)
            std::string json = std::string("{") +
                "\"id\":" + std::to_string(persisted_msg_id) + "," +
                "\"sender\":\"" + sender + "\"," +
                "\"text\":\"" + text + "\"," +
                "\"ts_ms\":" + std::to_string(now64) + "}";
            std::string key = std::string("room:") + persisted_room_id + ":recent";
            if (!redis_->lpush_trim(key, json, 200)) {
                corelog::warn(std::string("Redis update failed for key=") + key);
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
        // 프레즌스(user) heartbeat(옵션 TTL 갱신)
        if (redis_) {
            try {
                std::string uid; { std::lock_guard<std::mutex> lk(state_.mu); auto it = state_.user_uuid.find(session_sp.get()); if (it != state_.user_uuid.end()) uid = it->second; }
                if (!uid.empty()) {
                    unsigned int ttl = 30; if (const char* v = std::getenv("PRESENCE_TTL_SEC")) { unsigned long t = std::strtoul(v, nullptr, 10); if (t > 0 && t < 3600) ttl = static_cast<unsigned int>(t); }
                    std::string pfx; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) pfx = p;
                    std::string key = pfx + std::string("presence:user:") + uid; redis_->setex(key, "1", ttl);
                }
            } catch (...) {}
        }
        // Redis Pub/Sub 분산 브로드캐스트(옵션)
        if (redis_) {
            try {
                if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
                    std::string pfx; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) pfx = p;
                    std::string ch = pfx + std::string("fanout:room:") + current_room;
                    redis_->publish(ch, std::string(bytes.begin(), bytes.end()));
                }
            } catch (...) {}
        }
        // 본인이 보낸 메시지는 읽음 처리: last_seen = persisted_msg_id
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

} // namespace server::app::chat

