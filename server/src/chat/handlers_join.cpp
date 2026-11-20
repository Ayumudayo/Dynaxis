#include "server/chat/chat_service.hpp"

#include "chat_room_state.hpp"
#include "chat_service_state.hpp"

#include "server/core/concurrent/job_queue.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/storage/connection_pool.hpp"
#include "server/storage/redis/client.hpp"
#include "wire.pb.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

void ChatService::on_join(ChatService::NetSession& s, std::span<const std::uint8_t> payload) {
    std::string room;
    std::string password_field;
    if (!proto::read_lp_utf8(payload, room)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad join payload");
        return;
    }
    if (!payload.empty()) {
        proto::read_lp_utf8(payload, password_field);
    }

    auto session_sp = s.shared_from_this();
    if (!enqueue_session(session_sp, [this, session_sp, room, password = std::move(password_field)]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string user_uuid;
        std::string provided_password = password;
        std::string joined_room_id;
        std::string previous_room;
        std::string sender;
        std::string room_to_join = room.empty() ? std::string("lobby") : room;
        std::string logical_session_id;
        std::uint64_t logical_session_expires_unix_ms = 0;
        corelog::info(std::string("JOIN_ROOM: ") + room_to_join);

        if (!admit_shared_room(*session_sp, room_to_join, provided_password)) return;
        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            if (!impl_->state.authed.count(session_sp.get())) {
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized");
                return;
            }
            sender = impl_->state.user[session_sp.get()];
            auto current_room_it = impl_->state.cur_room.find(session_sp.get());
            if (current_room_it != impl_->state.cur_room.end()) {
                previous_room = current_room_it->second;
            }
            if (current_room_it != impl_->state.cur_room.end() && current_room_it->second != room_to_join) {
                remove_session_from_room_locked(impl_->state, session_sp, current_room_it->second, sender);
            }

            place_session_in_room_locked(impl_->state, session_sp, room_to_join, sender, false, false);

            if (auto uuid_it = impl_->state.user_uuid.find(session_sp.get()); uuid_it != impl_->state.user_uuid.end()) {
                user_uuid = uuid_it->second;
            }
            if (auto logical_it = impl_->state.logical_session_id.find(session_sp.get()); logical_it != impl_->state.logical_session_id.end()) {
                logical_session_id = logical_it->second;
            }
            if (auto expires_it = impl_->state.logical_session_expires_unix_ms.find(session_sp.get());
                expires_it != impl_->state.logical_session_expires_unix_ms.end()) {
                logical_session_expires_unix_ms = expires_it->second;
            }

            server::wire::v1::ChatBroadcast pb;
            pb.set_room(room_to_join);
            pb.set_sender("(system)");
            pb.set_text(sender + " 님이 입장했습니다");
            pb.set_sender_sid(0);
            const auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            pb.set_ts_ms(static_cast<std::uint64_t>(now64));
            std::string bytes;
            pb.SerializeToString(&bytes);
            body.assign(bytes.begin(), bytes.end());

            targets = collect_room_targets_locked(impl_->state, room_to_join, sender);
        }

        if (!logical_session_id.empty()) {
            persist_continuity_room(logical_session_id, room_to_join, logical_session_expires_unix_ms);
        }

        for (auto& target : targets) {
            target->async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);
        }

        if (impl_->runtime.db_pool) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    auto it = impl_->state.user_uuid.find(session_sp.get());
                    if (it != impl_->state.user_uuid.end()) {
                        uid = it->second;
                    }
                }
                if (!uid.empty()) {
                    auto rid = ensure_room_id_ci(room_to_join);
                    if (!rid.empty()) {
                        joined_room_id = rid;
                        auto uow = impl_->runtime.db_pool->make_repository_unit_of_work();
                        uow->memberships().upsert_join(uid, rid, "member");
                        auto last_id = uow->messages().get_last_id(rid);
                        if (last_id > 0) {
                            uow->memberships().update_last_seen(uid, rid, last_id);
                        }
                        uow->commit();
                    }
                }
            } catch (...) {}
        }

        if (impl_->runtime.redis) {
            try {
                if (!previous_room.empty() && previous_room != room_to_join) {
                    impl_->runtime.redis->srem("room:users:" + previous_room, sender);
                }

                impl_->runtime.redis->sadd("room:users:" + room_to_join, sender);
                if (room_to_join != "lobby") {
                    impl_->runtime.redis->sadd("rooms:active", room_to_join);
                }
                if (!user_uuid.empty() && !joined_room_id.empty()) {
                    impl_->runtime.redis->sadd(make_presence_key("presence:room:", joined_room_id), user_uuid);
                }
            } catch (const std::exception& e) {
                corelog::error("DEBUG: Redis update failed in on_join: " + std::string(e.what()));
            } catch (...) {
                corelog::error("DEBUG: Redis update failed in on_join: unknown error");
            }
        } else {
            corelog::warn("DEBUG: Redis not available in on_join");
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

        send_snapshot(*session_sp, room_to_join);
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
