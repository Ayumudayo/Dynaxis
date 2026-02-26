#include "server/chat/chat_service.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
#include <cstdlib>
#include <optional>
#include "server/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
#include "server/core/storage/connection_pool.hpp"

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

/**
 * @brief 방 퇴장(leave) 핸들러 구현입니다.
 *
 * 룸 멤버십 정리와 presence/DB 상태 갱신을 함께 수행해,
 * 연결 종료 직후에도 룸 목록과 사용자 표시가 빠르게 수렴하도록 합니다.
 */
namespace server::app::chat {

// -----------------------------------------------------------------------------
// 방 퇴장 (Leave) 핸들러
// -----------------------------------------------------------------------------
// 사용자가 현재 방에서 나갈 때 호출됩니다.
void ChatService::on_leave(ChatService::NetSession& s, std::span<const std::uint8_t> payload) {
    auto session_sp = s.shared_from_this();
    std::string room;
    if (!payload.empty()) {
        proto::read_lp_utf8(payload, room);
    }

    // DB, Redis, fanout 처리까지 필요하므로 job_queue에서 비동기로 처리한다.
    if (!job_queue_.TryPush([this, session_sp, room]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string user_uuid;
        std::string room_uuid;
        const std::string next_room = "lobby";
        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        std::string room_to_leave;
        std::string sender_name;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            // 인증 여부 확인
            if (!state_.authed.count(session_sp.get())) {
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized");
                return;
            }
            // 현재 참여 중인 방 확인
            auto itcr = state_.cur_room.find(session_sp.get());
            if (itcr == state_.cur_room.end()) {
                session_sp->send_error(proto::errc::NO_ROOM, "no current room");
                return;
            }
            // 요청된 방 이름이 있다면 현재 방과 일치하는지 검증
            if (!room.empty() && itcr->second != room) {
                session_sp->send_error(proto::errc::ROOM_MISMATCH, "room mismatch");
                return;
            }
            room_to_leave = itcr->second;
            
            // 방에서 세션 제거
            auto itroom = state_.rooms.find(room_to_leave);
            if (itroom != state_.rooms.end()) {
                auto it2 = state_.user.find(session_sp.get());
                sender_name = (it2 != state_.user.end()) ? it2->second : std::string("guest");
                if (auto it_uuid = state_.user_uuid.find(session_sp.get()); it_uuid != state_.user_uuid.end()) { user_uuid = it_uuid->second; }
                const bool was_owner =
                    (room_to_leave != "lobby") &&
                    (state_.room_owners.find(room_to_leave) != state_.room_owners.end()) &&
                    (state_.room_owners[room_to_leave] == sender_name);
                itroom->second.erase(session_sp);
                
                // 퇴장 알림을 보낼 대상(방에 남은 사람들) 수집
                auto itb = state_.rooms.find(room_to_leave);
                if (itb != state_.rooms.end()) {
                    auto& set = itb->second;
                    collect_room_sessions(set, targets);
                    // 방이 비었으면 방 정보와 비밀번호 삭제 (로비 제외)
                    if (set.empty() && room_to_leave != std::string("lobby")) { 
                        state_.rooms.erase(itb); 
                        state_.room_passwords.erase(room_to_leave);
                        state_.room_owners.erase(room_to_leave);
                        state_.room_invites.erase(room_to_leave);
                    } else if (was_owner && room_to_leave != std::string("lobby")) {
                        std::string new_owner;
                        for (const auto& weak : set) {
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
                            state_.room_owners[room_to_leave] = new_owner;
                        }
                    }
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
                        blk_it != state_.user_blacklists.end() && blk_it->second.count(sender_name) > 0) {
                        continue;
                    }
                    filtered_targets.push_back(target);
                }
                targets = std::move(filtered_targets);
            }
            // 사용자를 로비로 이동시킴
            state_.cur_room[session_sp.get()] = std::string("lobby");
            state_.rooms["lobby"].insert(session_sp);
        }

        // 방 퇴장 브로드캐스트 메시지를 구성한다.
        // 해당 방 참여자에게 퇴장 알림을 먼저 팬아웃한다.
        if (!room_to_leave.empty()) {
            server::wire::v1::ChatBroadcast pb;
            pb.set_room(room_to_leave);
            pb.set_sender("(system)");
            pb.set_text(sender_name + " 님이 떠났습니다");
            pb.set_sender_sid(0);
            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            pb.set_ts_ms(static_cast<std::uint64_t>(now64));
            std::string bytes; pb.SerializeToString(&bytes);
            body.assign(bytes.begin(), bytes.end());
            for (auto& t : targets) {
                auto f = (t.get() == session_sp.get()) ? proto::FLAG_SELF : 0;
                t->async_send(game_proto::MSG_CHAT_BROADCAST, body, f);
            }
        }

        // Redis presence SET에서도 사용자를 제거해 TTL 기반 알림과 일치시킨다.
        if (redis_ && !room_to_leave.empty()) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    auto rid = ensure_room_id_ci(room_to_leave);
                    room_uuid = rid;
                    if (!rid.empty()) {
                        redis_->srem(make_presence_key("presence:room:", rid), uid);
                        // 화면 표시용 닉네임 목록에서 제거
                        redis_->srem("room:users:" + room_to_leave, sender_name);
                    }
                } else {
                    // uid가 없더라도(게스트 등) room_uuid는 필요할 수 있음
                    room_uuid = ensure_room_id_ci(room_to_leave);
                    redis_->srem("room:users:" + room_to_leave, sender_name);
                }
                
                // 방이 비었는지 확인하고 활성 목록에서 제거 (Room List Sync)
                if (room_to_leave != "lobby") {
                    std::size_t remaining = 1;
                    (void)redis_->scard("room:users:" + room_to_leave, remaining);
                    
                    // 디버그 로그: 남은 인원 확인

                    if (remaining == 0) {
                        redis_->srem("rooms:active", room_to_leave);
                        redis_->del("room:password:" + room_to_leave);
                        
                        // 방이 완전히 비었으므로 방을 비활성화(Close) 처리 (Room Rotation)
                        // 이렇게 하면 다음에 같은 이름으로 방을 만들어도 새로운 UUID가 발급되어
                        // 이전 채팅 내역이 보이지 않게 됩니다. (Privacy + Audit Log Preservation)
                        if (db_pool_ && !room_uuid.empty()) {
                            try {
                                auto uow = db_pool_->make_unit_of_work();
                                uow->rooms().close(room_uuid);
                                uow->commit();

                                // 로컬 캐시에서도 제거하여 다음번 방 생성 시 새로운 UUID를 발급받도록 함
                                {
                                    std::lock_guard<std::mutex> lk(state_.mu);
                                    state_.room_ids.erase(room_to_leave);
                                    state_.room_owners.erase(room_to_leave);
                                    state_.room_invites.erase(room_to_leave);
                                }
                            } catch (const std::exception& e) {
                                corelog::error("failed to close room: " + std::string(e.what()));
                            }
                        }
                    }
                }
                
                // 로비로 재입장하므로 Redis lobby에도 추가
                redis_->sadd("room:users:lobby", sender_name);
            } catch (...) {}
        }

        // 로비 입장 알림 메시지를 전송한다.
        // 로비에 입장했다는 알림을 전체 로비 인원에게 재전송한다.
        std::vector<std::shared_ptr<Session>> t2;
        std::vector<std::uint8_t> body2;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto itb = state_.rooms.find("lobby");
            if (itb != state_.rooms.end()) {
                auto& set = itb->second;
                collect_room_sessions(set, t2);
            }

            std::vector<std::shared_ptr<Session>> filtered_lobby_targets;
            filtered_lobby_targets.reserve(t2.size());
            for (auto& target : t2) {
                auto receiver_it = state_.user.find(target.get());
                if (receiver_it == state_.user.end()) {
                    continue;
                }
                const std::string& receiver = receiver_it->second;
                if (auto blk_it = state_.user_blacklists.find(receiver);
                    blk_it != state_.user_blacklists.end() && blk_it->second.count(sender_name) > 0) {
                    continue;
                }
                filtered_lobby_targets.push_back(target);
            }
            t2 = std::move(filtered_lobby_targets);
        }
        
        server::wire::v1::ChatBroadcast pb2;
        pb2.set_room("lobby");
        pb2.set_sender("(system)");
        pb2.set_text(sender_name + " 님이 입장했습니다");
        pb2.set_sender_sid(0);
        {
            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            pb2.set_ts_ms(static_cast<std::uint64_t>(now64));
        }
        std::string bytes2; pb2.SerializeToString(&bytes2);
        body2.assign(bytes2.begin(), bytes2.end());
        for (auto& t : t2) t->async_send(game_proto::MSG_CHAT_BROADCAST, body2, 0);

        // 로비와 해당 방에 있는 다른 유저들에게 새로고침 알림 전송
        broadcast_refresh("lobby");
        if (!room_to_leave.empty() && room_to_leave != "lobby") {
            broadcast_refresh(room_to_leave);
        }

        // Write-behind 이벤트 발행
        if (!room_to_leave.empty()) {
            std::optional<std::string> uid_opt;
            if (!user_uuid.empty()) {
                uid_opt = user_uuid;
            }
            std::optional<std::string> room_id_opt;
            if (!room_uuid.empty()) {
                room_id_opt = room_uuid;
            } else {
                 auto rid = ensure_room_id_ci(room_to_leave);
                 if (!rid.empty()) room_id_opt = rid;
            }
            
            std::vector<std::pair<std::string, std::string>> wb_fields;
            wb_fields.emplace_back("room", room_to_leave);
            wb_fields.emplace_back("user_name", sender_name);
            wb_fields.emplace_back("next_room", next_room);
            // DLQ/재처리를 위해 leave 이벤트 메타데이터를 Redis stream에 남긴다.
            emit_write_behind_event("room_leave", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
        }

        // 마지막으로 로비 상태 스냅샷을 내려 클라이언트 UI를 즉시 업데이트한다.
        send_snapshot(*session_sp, next_room);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_leave dropped: job queue full");
    }
}

} // namespace server::app::chat
