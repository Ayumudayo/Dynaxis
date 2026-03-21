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
#include "server/storage/connection_pool.hpp"

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

/**
 * @brief 방 입장(join) 핸들러 구현입니다.
 *
 * 비밀번호 검증/멤버십 기록/presence 갱신/입장 브로드캐스트를
 * 단일 작업 단위로 처리해 상태 불일치를 최소화합니다.
 * join은 보기보다 쓰기 단계가 많아서, 중간에 순서가 틀어지면
 * "방에는 들어왔는데 소유자/비밀번호/활성 목록은 이전 값" 같은 반쪽 상태가 쉽게 생깁니다.
 */
namespace server::app::chat {

// join은 한 사용자의 위치를 한 방에서 다른 방으로 옮기는 authoritative 전이다.
// 비밀번호, 멤버십, recent-history 기준점, presence, fanout이 같은 순서를 따라야 반쪽 상태가 남지 않는다.
void ChatService::on_join(ChatService::NetSession& s, std::span<const std::uint8_t> payload) {
    std::string room;
    std::string sp;
    if (!proto::read_lp_utf8(payload, room)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad join payload");
        return;
    }
    // room 이름을 읽고 나면 span은 자동으로 다음 필드 위치로 이동한다.
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
        std::string logical_session_id;
        std::uint64_t logical_session_expires_unix_ms = 0;
        if (room_to_join.empty()) room_to_join = "lobby";
        corelog::info(std::string("JOIN_ROOM: ") + room_to_join);

        // 1. Redis에서 비밀번호를 먼저 읽는다.
        // 느린 원격 조회를 state mutex 안에서 수행하면 전체 방 상태 갱신이 막히므로 락 밖에서 처리한다.
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
            
            // 비밀번호 검증은 Redis -> 로컬 캐시 순으로 확인한다.
            // 분산 상태를 우선 존중하되, Redis 일시 실패 시 로컬 캐시로 완전히 빈 방처럼 오판하지 않게 한다.
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

            // 기존 방에서 먼저 제거하고 새 방에 추가해야 한 세션이 두 방의 active member로 동시에 남지 않는다.
            auto itold = state_.cur_room.find(session_sp.get());
            if (itold != state_.cur_room.end()) { previous_room = itold->second; }
            if (itold != state_.cur_room.end() && itold->second != room_to_join) {
                auto itroom = state_.rooms.find(itold->second);
                if (itroom != state_.rooms.end()) {
                    const std::string old_room = itold->second;
                    itroom->second.erase(session_sp);
                    // 방에 남아 있는 세션이 없다면(로비 제외) 방 메타데이터도 함께 정리한다.
                    // 빈 방 흔적을 남겨 두면 같은 이름으로 재생성할 때 이전 소유자/비밀번호가 섞일 수 있다.
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
            
            // 새 방 등록은 기존 방 정리 뒤에 수행해 room membership이 한 시점 기준으로 보이게 한다.
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
            
            // 입장 알림 본문은 lock 안에서 구성하되, 실제 전송은 lock 밖에서 수행한다.
            // 전송 중 네트워크 backpressure가 생겨도 공용 상태 락을 오래 잡지 않기 위해서다.
            if (auto it_uuid = state_.user_uuid.find(session_sp.get()); it_uuid != state_.user_uuid.end()) { user_uuid = it_uuid->second; }
            if (auto it_logical = state_.logical_session_id.find(session_sp.get()); it_logical != state_.logical_session_id.end()) {
                logical_session_id = it_logical->second;
            }
            if (auto it_expires = state_.logical_session_expires_unix_ms.find(session_sp.get());
                it_expires != state_.logical_session_expires_unix_ms.end()) {
                logical_session_expires_unix_ms = it_expires->second;
            }
            
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
        if (!logical_session_id.empty()) {
            persist_continuity_room(logical_session_id, room_to_join, logical_session_expires_unix_ms);
        }

        // 로컬 세션들에게 입장 알림 전송
        for (auto& t : targets) t->async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);

        // DB 멤버십은 upsert로 반영한다.
        // join은 재시도나 중복 호출이 있을 수 있으므로, 중복 row보다 "최신 상태 하나"가 유지되는 편이 복구와 감사에 유리하다.
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
                        auto uow = db_pool_->make_repository_unit_of_work();
                        // 멤버십 테이블에는 입장 기록을 upsert한다.
                        // join이 반복돼도 중복 row보다 최신 상태가 더 중요하므로 upsert가 맞다.
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

        // Redis presence와 room 사용자 목록은 DB와 독립적으로 갱신한다.
        // DB commit과 한 트랜잭션으로 묶을 수는 없지만, 적어도 같은 join 작업 안에서 순서를 고정해 수렴을 빠르게 만든다.
        if (redis_) {
            try {
                // 1. 이전 방에서 제거(방 이동 시)
                if (!previous_room.empty() && previous_room != room_to_join) {
                    redis_->srem("room:users:" + previous_room, sender);
                    
                    // 이전 방이 비었는지 확인하고 활성 목록에서 제거한다.
                    if (previous_room != "lobby") {
                        std::size_t remaining = 1;
                        if (redis_->scard("room:users:" + previous_room, remaining) && remaining == 0) {
                            redis_->srem("rooms:active", previous_room);
                            redis_->del("room:password:" + previous_room);
                              if (db_pool_) {
                                try {
                                    auto uow = db_pool_->make_repository_unit_of_work();
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

                // 2.1 활성 방 목록에 추가한다.
                if (room_to_join != "lobby") {
                    redis_->sadd("rooms:active", room_to_join);
                }

                // 3. presence 갱신(로그인 사용자만)
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
        
        // join도 나중에 감사나 DLQ 재처리로 따라가야 하므로 stream에 남긴다.
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
