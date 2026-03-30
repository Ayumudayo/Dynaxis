#include "server/chat/chat_service.hpp"
#include "chat_service_state.hpp"
#include "server/chat/chat_hook_plugin_abi.hpp"
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
    // TCP 연결이 끊긴 직후 모든 정리를 끝내야, 좀비 세션과 stale presence가 오래 남지 않는다.
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
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            if (auto itname = impl_->state.user.find(s.get()); itname != impl_->state.user.end()) {
                user_for_hook = itname->second;
            }
        }
        notify_session_event_hook(s->session_id(),
                                  SessionEventKindV2::kClose,
                                  user_for_hook,
                                  "connection_closed");

        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            impl_->state.by_session_id.erase(s->session_id());
            if (auto itname = impl_->state.user.find(s.get()); itname != impl_->state.user.end()) {
                name = itname->second;
            } else {
                name = "guest";
            }
            impl_->state.authed.erase(s.get());
            impl_->state.guest.erase(s.get());
            if (auto it_uuid = impl_->state.user_uuid.find(s.get()); it_uuid != impl_->state.user_uuid.end()) { user_uuid = it_uuid->second; }
            if (!name.empty()) {
                auto itset = impl_->state.by_user.find(name);
                if (itset != impl_->state.by_user.end()) { itset->second.erase(s); }
            }
            impl_->state.user.erase(s.get());
            // 세션 UUID 캐시도 더 이상 필요 없으므로 제거한다.
            impl_->state.session_uuid.erase(s.get());
            impl_->state.logical_session_id.erase(s.get());
            impl_->state.logical_session_expires_unix_ms.erase(s.get());
            impl_->state.cur_world.erase(s.get());
            impl_->state.session_ip.erase(s.get());
            impl_->state.session_hwid_hash.erase(s.get());
            auto itcr = impl_->state.cur_room.find(s.get());
            if (itcr != impl_->state.cur_room.end()) {
                room_left = itcr->second;
                auto itroom = impl_->state.rooms.find(room_left);
                if (itroom != impl_->state.rooms.end()) {
                    const bool was_owner =
                        (room_left != "lobby") &&
                        (impl_->state.room_owners.find(room_left) != impl_->state.room_owners.end()) &&
                        (impl_->state.room_owners[room_left] == name);
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
                    auto itb = impl_->state.rooms.find(room_left);
                    if (itb != impl_->state.rooms.end()) {
                        auto& set = itb->second;
                        collect_room_sessions(set, targets);
                        if (set.empty() && room_left != std::string("lobby")) {
                            impl_->state.rooms.erase(itb);
                            impl_->state.room_passwords.erase(room_left);
                            impl_->state.room_owners.erase(room_left);
                            impl_->state.room_invites.erase(room_left);
                        } else if (was_owner && room_left != std::string("lobby")) {
                            std::string new_owner;
                            for (const auto& weak : set) {
                                if (auto candidate = weak.lock()) {
                                    auto user_it = impl_->state.user.find(candidate.get());
                                    if (user_it != impl_->state.user.end()) {
                                        new_owner = user_it->second;
                                        if (new_owner != "guest") {
                                            break;
                                        }
                                    }
                                }
                            }
                            if (!new_owner.empty()) {
                                impl_->state.room_owners[room_left] = new_owner;
                            }
                        }
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
                            blk_it != impl_->state.user_blacklists.end() && blk_it->second.count(name) > 0) {
                            continue;
                        }
                        filtered_targets.push_back(target);
                    }
                    targets = std::move(filtered_targets);
                }
                impl_->state.cur_room.erase(itcr);
            }
        }
        for (auto& t : targets) { t->async_send(game_proto::MSG_CHAT_BROADCAST, body, 0); }
        // Redis 프레즌스 SET에서 사용자를 제거한다.
        if (impl_->runtime.redis && !room_left.empty()) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    auto it = impl_->state.user_uuid.find(s.get());
                    if (it != impl_->state.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    auto rid = ensure_room_id_ci(room_left);
                    room_uuid = rid;
                    if (!rid.empty()) {
                        impl_->runtime.redis->srem(make_presence_key("presence:room:", rid), uid);
                    }
                }
                // 화면 표시용 닉네임 목록에서 제거
                impl_->runtime.redis->srem("room:users:" + room_left, name);
                
                // 연결 끊김 정리 중에는 잠깐 로비로 이동한 것으로 간주한다.
                // 실제로는 곧 완전히 끊기지만, 정리 중 다른 사용자 화면이 급격히 흔들리지 않게 하는 타협이다.
                if (room_left != "lobby") {
                    impl_->runtime.redis->sadd("room:users:lobby", name);
                }

                // 방이 비었는지 확인하고 활성 목록에서 제거한다.
                if (room_left != "lobby") {
                    std::size_t remaining = 1;
                    if (impl_->runtime.redis->scard("room:users:" + room_left, remaining) && remaining == 0) {
                        impl_->runtime.redis->srem("rooms:active", room_left);
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
            // `session_close` 이벤트를 stream에 남겨 재처리와 감사에 활용한다.
            emit_write_behind_event("session_close", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
            
            // 해당 방의 다른 유저들에게 갱신 알림
            broadcast_refresh(room_left);
        }
    });
}

} // namespace server::app::chat
