#include "server/chat/chat_service.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "wire.pb.h"
#include <cstdlib>
#include "server/storage/redis/client.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include <optional>

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;

/**
 * @brief 세션 종료 시 후처리(cleanup/fanout/audit) 구현입니다.
 *
 * 연결 종료 이벤트를 룸/프레즌스/스토리지 관점에서 정리해,
 * 좀비 세션과 stale presence를 빠르게 제거하고 감사 추적성을 유지합니다.
 */
namespace server::app::chat {

void ChatService::on_session_close(std::shared_ptr<Session> s) {
    // 세션 종료 시에는 Redis/DB 정리와 방 브로드캐스트가 필요하므로 worker 큐에서 처리한다.
    // TCP 연결이 끊어지면 즉시 호출되며, 여기서 모든 정리 작업을 수행해야 좀비 세션이 남지 않습니다.
    job_queue_.Push([this, s]() {
        const std::string session_id_str = get_or_create_session_uuid(*s);
        std::string user_uuid;
        std::string room_uuid;
        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        std::string name;
        std::string room_left;

        std::string user_for_hook;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (auto itname = state_.user.find(s.get()); itname != state_.user.end()) {
                user_for_hook = itname->second;
            }
        }
        notify_session_event_hook(s->session_id(),
                                  SessionEventKindV2::kClose,
                                  user_for_hook,
                                  "connection_closed");

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            state_.by_session_id.erase(s->session_id());
            if (auto itname = state_.user.find(s.get()); itname != state_.user.end()) {
                name = itname->second;
            } else {
                name = "guest";
            }
            state_.authed.erase(s.get());
            state_.guest.erase(s.get());
            if (auto it_uuid = state_.user_uuid.find(s.get()); it_uuid != state_.user_uuid.end()) { user_uuid = it_uuid->second; }
            if (!name.empty()) {
                auto itset = state_.by_user.find(name);
                if (itset != state_.by_user.end()) { itset->second.erase(s); }
            }
            state_.user.erase(s.get());
            // 세션 UUID 캐시도 더 이상 필요 없으므로 제거한다.
            state_.session_uuid.erase(s.get());
            state_.logical_session_id.erase(s.get());
            state_.logical_session_expires_unix_ms.erase(s.get());
            state_.cur_world.erase(s.get());
            state_.session_ip.erase(s.get());
            state_.session_hwid_hash.erase(s.get());
            auto itcr = state_.cur_room.find(s.get());
            if (itcr != state_.cur_room.end()) {
                room_left = itcr->second;
                auto itroom = state_.rooms.find(room_left);
                if (itroom != state_.rooms.end()) {
                    const bool was_owner =
                        (room_left != "lobby") &&
                        (state_.room_owners.find(room_left) != state_.room_owners.end()) &&
                        (state_.room_owners[room_left] == name);
                    itroom->second.erase(s);
                    server::wire::v1::ChatBroadcast pb;
                    pb.set_room(room_left);
                    pb.set_sender("(system)");
                    pb.set_text(name + std::string(" 님이 연결을 종료했습니다"));
                    pb.set_sender_sid(0);
                    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    pb.set_ts_ms(static_cast<std::uint64_t>(now64));
                    std::string bytes; pb.SerializeToString(&bytes);
                    body.assign(bytes.begin(), bytes.end());
                    auto itb = state_.rooms.find(room_left);
                    if (itb != state_.rooms.end()) {
                        auto& set = itb->second;
                        collect_room_sessions(set, targets);
                        if (set.empty() && room_left != std::string("lobby")) {
                            state_.rooms.erase(itb);
                            state_.room_passwords.erase(room_left);
                            state_.room_owners.erase(room_left);
                            state_.room_invites.erase(room_left);
                        } else if (was_owner && room_left != std::string("lobby")) {
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
                                state_.room_owners[room_left] = new_owner;
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
                            blk_it != state_.user_blacklists.end() && blk_it->second.count(name) > 0) {
                            continue;
                        }
                        filtered_targets.push_back(target);
                    }
                    targets = std::move(filtered_targets);
                }
                state_.cur_room.erase(itcr);
            }
        }
        for (auto& t : targets) { t->async_send(game_proto::MSG_CHAT_BROADCAST, body, 0); }
        // Redis 프레즌스 SET에서 사용자를 제거한다.
        if (redis_ && !room_left.empty()) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(s.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    auto rid = ensure_room_id_ci(room_left);
                    room_uuid = rid;
                    if (!rid.empty()) {
                        redis_->srem(make_presence_key("presence:room:", rid), uid);
                    }
                }
                // 화면 표시용 닉네임 목록에서 제거
                redis_->srem("room:users:" + room_left, name);
                
                // 연결 끊김 전에 로비로 이동한다고 간주 (다른 사용자들에게 로비 표시)
                // 실제로는 곧 완전히 끊어지지만, 정리 과정에서 일시적으로 lobby에 추가
                if (room_left != "lobby") {
                    redis_->sadd("room:users:lobby", name);
                }
                
                // 방이 비었는지 확인하고 활성 목록에서 제거 (Room List Sync)
                if (room_left != "lobby") {
                    std::size_t remaining = 1;
                    if (redis_->scard("room:users:" + room_left, remaining) && remaining == 0) {
                        redis_->srem("rooms:active", room_left);
                    }
                }
            } catch (...) {}
        }
        if (!room_left.empty()) {
            std::optional<std::string> uid_opt;
            if (!user_uuid.empty()) {
                uid_opt = user_uuid;
            }
            std::optional<std::string> room_id_opt;
            if (!room_uuid.empty()) {
                room_id_opt = room_uuid;
            }
            std::vector<std::pair<std::string, std::string>> wb_fields;
            wb_fields.emplace_back("room", room_left);
            wb_fields.emplace_back("user_name", name);
            // session_close 이벤트를 stream에 남겨 재처리/감사에 활용한다.
            emit_write_behind_event("session_close", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
            
            // 해당 방의 다른 유저들에게 갱신 알림
            broadcast_refresh(room_left);
        }
    });
}

} // namespace server::app::chat
