// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
#include <cstdlib>
#include <optional>
#include "server/storage/redis/client.hpp"
// storage
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

void ChatService::on_join(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string room;
    if (!proto::read_lp_utf8(sp, room)) { 
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad join payload"); 
        return; 
    }
    std::string password;
    if (!sp.empty()) {
        if (!proto::read_lp_utf8(sp, password)) {
            s.send_error(proto::errc::INVALID_PAYLOAD, "bad join payload");
            return;
        }
    }

    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, room, password]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string user_uuid;
        std::string provided_password = password;
        std::string joined_room_id;
        std::string previous_room;
        std::string sender;
        std::string room_to_join = room;
        if (room_to_join.empty()) room_to_join = "lobby";
        corelog::info(std::string("JOIN_ROOM: ") + room_to_join);

        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }
            bool room_exists = state_.rooms.find(room_to_join) != state_.rooms.end();
            auto pass_it = state_.room_passwords.find(room_to_join);
            if (pass_it != state_.room_passwords.end()) {
                const std::string& expected = pass_it->second;
                if (provided_password.empty() || hash_room_password(provided_password) != expected) {
                    session_sp->send_error(proto::errc::FORBIDDEN, "room locked");
                    return;
                }
            } else if (!provided_password.empty() && room_to_join != "lobby") {
                state_.room_passwords[room_to_join] = hash_room_password(provided_password);
            }
            // 기존 방에서 제거
            auto itold = state_.cur_room.find(session_sp.get());
            if (itold != state_.cur_room.end()) { previous_room = itold->second; }
            if (itold != state_.cur_room.end() && itold->second != room_to_join) {
                auto itroom = state_.rooms.find(itold->second);
                if (itroom != state_.rooms.end()) {
                    itroom->second.erase(session_sp);
                    // 기존 방이 비면(lobby 제외) 제거
                    bool is_empty = true;
                    for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                        if (wit->expired()) wit = itroom->second.erase(wit); 
                        else { is_empty = false; ++wit; }
                    }
                    if (is_empty && itold->second != "lobby") {
                        state_.rooms.erase(itroom);
                        state_.room_passwords.erase(itold->second);
                    }
                }
            }
            state_.cur_room[session_sp.get()] = room_to_join;
            state_.rooms[room_to_join].insert(session_sp);
            // 입장 브로드캐스트(Protobuf)
            auto it2 = state_.user.find(session_sp.get()); 
            sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
            if (auto it_uuid = state_.user_uuid.find(session_sp.get()); it_uuid != state_.user_uuid.end()) { user_uuid = it_uuid->second; }
            server::wire::v1::ChatBroadcast pb; 
            pb.set_room(room_to_join); 
            pb.set_sender("(system)"); 
            pb.set_text(sender + " 님이 입장했습니다"); 
            pb.set_sender_sid(0);
            {
                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                pb.set_ts_ms(static_cast<std::uint64_t>(now64));
            }
            {
                std::string bytes; pb.SerializeToString(&bytes);
                body.assign(bytes.begin(), bytes.end());
            }
            // 타겟 수집
            auto it = state_.rooms.find(room_to_join);
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
        for (auto& t : targets) t->async_send(proto::MSG_CHAT_BROADCAST, body, 0);

        // 멤버십 영속화(upsert) + Redis 프레즌스
        if (db_pool_) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    auto rid = ensure_room_id_ci(room_to_join);
                    if (!rid.empty()) {
                        joined_room_id = rid;
                        auto uow = db_pool_->make_unit_of_work();
                        uow->memberships().upsert_join(uid, rid, "member");
                        // 방 입장 시점의 마지막 메시지까지 읽음 처리
                        auto last_id = uow->messages().get_last_id(rid);
                        if (last_id > 0) {
                            uow->memberships().update_last_seen(uid, rid, last_id);
                        }
                        uow->commit();
                        if (redis_) {
                            std::string pfx; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) pfx = p;
                            std::string pkey = pfx + std::string("presence:room:") + rid;
                            redis_->sadd(pkey, uid);
                        }
                    }
                }
            } catch (...) {}
        }
        std::optional<std::string> uid_opt;
        if (!user_uuid.empty()) {
            uid_opt = user_uuid;
        }
        std::optional<std::string> room_id_opt;
        if (!joined_room_id.empty()) {
            room_id_opt = joined_room_id;
        }
        std::vector<std::pair<std::string, std::string>> wb_fields;
        wb_fields.emplace_back("room", room_to_join);
        wb_fields.emplace_back("user_name", sender);
        if (!previous_room.empty() && previous_room != room_to_join) {
            wb_fields.emplace_back("prev_room", previous_room);
        }
        emit_write_behind_event("room_join", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
    });
}

} // namespace server::app::chat
