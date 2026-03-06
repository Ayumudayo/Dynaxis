#include "server/chat/chat_service.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
#include <cstdlib>
#include <optional>
#include "server/storage/redis/client.hpp"
// 저장소 연동 헤더
#include "server/core/storage/connection_pool.hpp"

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

/**
 * @brief 방 입장(join) 핸들러 구현입니다.
 *
 * 비밀번호 검증/멤버십 기록/presence 갱신/입장 브로드캐스트를
 * 단일 작업 단위로 처리해 상태 불일치를 최소화합니다.
 */
namespace server::app::chat {

// -----------------------------------------------------------------------------
// 방 입장 (Join) 핸들러
// -----------------------------------------------------------------------------
// 사용자가 특정 방에 입장을 요청할 때 호출됩니다.
void ChatService::on_join(ChatService::NetSession& s, std::span<const std::uint8_t> payload) {
    std::string room;
    std::string sp;
    if (!proto::read_lp_utf8(payload, room)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad join payload");
        return;
    }
    // payload is now advanced past room
    if (!payload.empty()) {
        proto::read_lp_utf8(payload, sp);
    }
    std::string password = sp;

    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp, room, password]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string user_uuid;
        std::string provided_password = password;
        std::string joined_room_id;
        std::string previous_room;
        std::string sender;
        std::string room_to_join = room;
        if (room_to_join.empty()) room_to_join = "lobby";
        corelog::info(std::string("JOIN_ROOM: ") + room_to_join);

        // 1. Redis에서 비밀번호 미리 조회 (Lock 없이 수행)
        std::string redis_password_value;
        bool redis_password_found = false;
        if (redis_) {
            auto pw = redis_->get("room:password:" + room_to_join);
            if (pw.has_value()) {
                redis_password_value = pw.value();
                redis_password_found = true;
            }
        }

        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        
        bool should_set_redis_password = false;
        std::string new_hashed_password;

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            // 인증된 세션인지 확인
            if (!state_.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }

            auto it2 = state_.user.find(session_sp.get());
            sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");

            if (maybe_handle_join_hook(*session_sp, sender, room_to_join)) {
                return;
            }

            const bool is_admin_user = admin_users_.count(sender) > 0;
            bool is_room_owner = false;
            bool has_room_invite = false;
            if (room_to_join != "lobby") {
                if (auto owner_it = state_.room_owners.find(room_to_join); owner_it != state_.room_owners.end()) {
                    is_room_owner = (owner_it->second == sender);
                }
                if (auto invite_it = state_.room_invites.find(room_to_join); invite_it != state_.room_invites.end()) {
                    has_room_invite = invite_it->second.count(sender) > 0;
                }
            }
            
            // 비밀번호 검증 로직
            std::string expected_password;
            
            // Redis 조회 결과 반영
            if (redis_password_found) {
                expected_password = redis_password_value;
                state_.room_passwords[room_to_join] = expected_password; // 로컬 캐시 갱신
            }
            
            // Redis에 없으면 로컬 확인
            if (expected_password.empty()) {
                auto pass_it = state_.room_passwords.find(room_to_join);
                if (pass_it != state_.room_passwords.end()) {
                    expected_password = pass_it->second;
                }
            }

            const bool has_password = !expected_password.empty();
            const bool password_ok = has_password && !provided_password.empty() && verify_room_password(provided_password, expected_password);
            const bool room_exists =
                state_.rooms.find(room_to_join) != state_.rooms.end() ||
                state_.room_owners.find(room_to_join) != state_.room_owners.end() ||
                state_.room_passwords.find(room_to_join) != state_.room_passwords.end() ||
                has_password;

            if (room_to_join != "lobby" && room_exists && !is_admin_user && !is_room_owner && !has_room_invite && !password_ok) {
                session_sp->send_error(proto::errc::FORBIDDEN, has_password ? "room locked" : "invite required");
                return;
            }

            if (has_password && password_ok) {
                if (!is_modern_room_password_hash(expected_password)) {
                    new_hashed_password = hash_room_password(provided_password);
                    if (!new_hashed_password.empty()) {
                        state_.room_passwords[room_to_join] = new_hashed_password;
                        should_set_redis_password = true;
                    }
                }
            } else if (!has_password && !provided_password.empty() && room_to_join != "lobby") {
                // 새 방이거나 비밀번호가 없는 방에 비밀번호를 설정하며 입장하는 경우
                new_hashed_password = hash_room_password(provided_password);
                if (new_hashed_password.empty()) {
                    session_sp->send_error(proto::errc::INTERNAL_ERROR, "password hash failed");
                    return;
                }
                state_.room_passwords[room_to_join] = new_hashed_password;
                should_set_redis_password = true;
            }

            // 기존에 참여 중인 방이 있으면 그 방에서 먼저 제거한다.
            auto itold = state_.cur_room.find(session_sp.get());
            if (itold != state_.cur_room.end()) { previous_room = itold->second; }
            if (itold != state_.cur_room.end() && itold->second != room_to_join) {
                auto itroom = state_.rooms.find(itold->second);
                if (itroom != state_.rooms.end()) {
                    const std::string old_room = itold->second;
                    itroom->second.erase(session_sp);
                    // 방에 남아 있는 세션이 없다면(로비 제외) 비밀번호도 함께 삭제한다.
                    // 빈 방의 상태를 정리하여 메모리 누수를 방지합니다.
                    bool is_empty = true;
                    for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                        if (wit->expired()) wit = itroom->second.erase(wit); 
                        else { is_empty = false; ++wit; }
                    }
                    if (is_empty && itold->second != "lobby") {
                        state_.rooms.erase(itroom);
                        state_.room_passwords.erase(itold->second);
                        state_.room_owners.erase(itold->second);
                        state_.room_invites.erase(itold->second);
                    } else if (!is_empty && old_room != "lobby") {
                        auto owner_it = state_.room_owners.find(old_room);
                        if (owner_it != state_.room_owners.end() && owner_it->second == sender) {
                            std::string new_owner;
                            for (const auto& weak : itroom->second) {
                                if (auto candidate = weak.lock()) {
                                    auto user_it = state_.user.find(candidate.get());
                                    if (user_it != state_.user.end()) {
                                        new_owner = user_it->second;
                                        if (new_owner != "guest") {
                                            break;
                                        }
                                    }
                                }
                            }
                            if (!new_owner.empty()) {
                                owner_it->second = new_owner;
                            }
                        }
                    }
                }
            }
            
            // 새 방에 세션 추가
            state_.cur_room[session_sp.get()] = room_to_join;
            state_.rooms[room_to_join].insert(session_sp);

            if (room_to_join != "lobby") {
                auto owner_it = state_.room_owners.find(room_to_join);
                if (owner_it == state_.room_owners.end() && sender != "guest") {
                    state_.room_owners[room_to_join] = sender;
                }
                if (has_room_invite) {
                    auto invite_it = state_.room_invites.find(room_to_join);
                    if (invite_it != state_.room_invites.end()) {
                        invite_it->second.erase(sender);
                        if (invite_it->second.empty()) {
                            state_.room_invites.erase(invite_it);
                        }
                    }
                }
            }
            
            // 입장 알림 브로드캐스트 메시지를 구성한다.
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
            // 브로드캐스트 대상 목록(weak_ptr)을 실제 세션 포인터로 정리한다.
            auto it = state_.rooms.find(room_to_join);
            if (it != state_.rooms.end()) {
                collect_room_sessions(it->second, targets);
            }

            std::vector<std::shared_ptr<Session>> filtered_targets;
            filtered_targets.reserve(targets.size());
            for (auto& target : targets) {
                auto receiver_it = state_.user.find(target.get());
                if (receiver_it == state_.user.end()) {
                    continue;
                }
                const std::string& receiver = receiver_it->second;
                if (auto blk_it = state_.user_blacklists.find(receiver);
                    blk_it != state_.user_blacklists.end() && blk_it->second.count(sender) > 0) {
                    continue;
                }
                filtered_targets.push_back(target);
            }
            targets = std::move(filtered_targets);
        }
        
        // Redis 비밀번호 설정 (Lock 해제 후 수행)
        if (should_set_redis_password && redis_) {
            redis_->setex("room:password:" + room_to_join, new_hashed_password, 86400); // 24시간 TTL
        }

        // 로컬 세션들에게 입장 알림 전송
        for (auto& t : targets) t->async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);

        // DB upsert (멤버십 기록)
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
                        // 멤버십 테이블에 입장 기록 (upsert)
                        uow->memberships().upsert_join(uid, rid, "member");
                        // 방 입장 시점의 마지막 메시지까지 읽음으로 표시한다.
                        auto last_id = uow->messages().get_last_id(rid);
                        if (last_id > 0) {
                            uow->memberships().update_last_seen(uid, rid, last_id);
                        }
                        uow->commit();
                    }
                }
            } catch (...) {}
        }

        // Redis Presence 및 User List 갱신 (DB와 독립적으로 수행)
        if (redis_) {
            try {
                // 1. 이전 방에서 제거 (방 이동 시)
                if (!previous_room.empty() && previous_room != room_to_join) {
                    redis_->srem("room:users:" + previous_room, sender);
                    
                    // 이전 방이 비었는지 확인하고 활성 목록에서 제거 (Room List Sync)
                    if (previous_room != "lobby") {
                        std::size_t remaining = 1;
                        if (redis_->scard("room:users:" + previous_room, remaining) && remaining == 0) {
                            redis_->srem("rooms:active", previous_room);
                            redis_->del("room:password:" + previous_room);
                              if (db_pool_) {
                                try {
                                    auto uow = db_pool_->make_unit_of_work();
                                    auto found = uow->rooms().find_by_name_exact_ci(previous_room);
                                    if (found) {
                                        uow->rooms().close(found->id);
                                        uow->commit();
                                    }
                                    {
                                        std::lock_guard<std::mutex> lk(state_.mu);
                                        state_.room_ids.erase(previous_room);
                                        state_.room_owners.erase(previous_room);
                                        state_.room_invites.erase(previous_room);
                                    }
                                } catch (...) {}
                            }
                        }
                    }
                }

                // 2. 새 방에 추가
                redis_->sadd("room:users:" + room_to_join, sender);

                // 2.1 활성 방 목록에 추가 (Room List Sync)
                if (room_to_join != "lobby") {
                    redis_->sadd("rooms:active", room_to_join);
                }

                // 3. Presence 갱신 (logged-in user only)
                if (!user_uuid.empty() && !joined_room_id.empty()) {
                    redis_->sadd(make_presence_key("presence:room:", joined_room_id), user_uuid);
                }
            } catch (const std::exception& e) {
                corelog::error("DEBUG: Redis update failed in on_join: " + std::string(e.what()));
            } catch (...) {
                corelog::error("DEBUG: Redis update failed in on_join: unknown error");
            }
        } else {
            corelog::warn("DEBUG: Redis not available in on_join");
        }
        
        // Write-behind 이벤트 발행
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

        // 새로운 방 상태를 즉시 고객에게 전달해 /refresh 없이도 UI를 갱신한다.
        send_snapshot(*session_sp, room_to_join);
        
        // 로비와 해당 방에 있는 다른 유저들에게 새로고침 알림 전송
        broadcast_refresh("lobby");
        if (room_to_join != "lobby") {
            broadcast_refresh(room_to_join);
        }
        if (!previous_room.empty() && previous_room != room_to_join && previous_room != "lobby") {
            broadcast_refresh(previous_room);
        }
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_join dropped: job queue full");
    }
}

} // namespace server::app::chat
