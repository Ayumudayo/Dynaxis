#include "server/chat/chat_service.hpp"

#include "chat_room_state.hpp"
#include "chat_service_state.hpp"

#include "server/core/concurrent/job_queue.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/storage/connection_pool.hpp"
#include "server/storage/redis/client.hpp"
#include "wire.pb.h"

#include <chrono>
#include <cstdlib>
#include <optional>

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

void ChatService::on_leave(ChatService::NetSession& s, std::span<const std::uint8_t> payload) {
    auto session_sp = s.shared_from_this();
    std::string room;
    if (!payload.empty()) {
        proto::read_lp_utf8(payload, room);
    }

    if (!job_queue_.TryPush([this, session_sp, room]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string user_uuid;
        std::string room_uuid;
        const std::string next_room = "lobby";
        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        std::string room_to_leave;
        std::string sender_name;
        std::string logical_session_id;
        std::uint64_t logical_session_expires_unix_ms = 0;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            if (!impl_->state.authed.count(session_sp.get())) {
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized");
                return;
            }

            auto current_room_it = impl_->state.cur_room.find(session_sp.get());
            if (current_room_it == impl_->state.cur_room.end()) {
                session_sp->send_error(proto::errc::NO_ROOM, "no current room");
                return;
            }
            if (!room.empty() && current_room_it->second != room) {
                session_sp->send_error(proto::errc::ROOM_MISMATCH, "room mismatch");
                return;
            }

            room_to_leave = current_room_it->second;
            if (room_exists_locked(impl_->state, room_to_leave)) {
                auto user_it = impl_->state.user.find(session_sp.get());
                sender_name = (user_it != impl_->state.user.end()) ? user_it->second : std::string("guest");
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

                if (maybe_handle_leave_hook(*session_sp, sender_name, room_to_leave)) {
                    return;
                }

                targets = remove_session_from_room_locked(impl_->state, session_sp, room_to_leave, sender_name).targets;
            }

            place_session_in_room_locked(impl_->state, session_sp, "lobby", sender_name);
        }

        if (!logical_session_id.empty()) {
            persist_continuity_room(logical_session_id, next_room, logical_session_expires_unix_ms);
        }

        if (!room_to_leave.empty()) {
            server::wire::v1::ChatBroadcast pb;
            pb.set_room(room_to_leave);
            pb.set_sender("(system)");
            pb.set_text(sender_name + " 님이 퇴장합니다");
            pb.set_sender_sid(0);
            const auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            pb.set_ts_ms(static_cast<std::uint64_t>(now64));
            std::string bytes;
            pb.SerializeToString(&bytes);
            body.assign(bytes.begin(), bytes.end());
            for (auto& target : targets) {
                auto flags = (target.get() == session_sp.get()) ? proto::FLAG_SELF : 0;
                target->async_send(game_proto::MSG_CHAT_BROADCAST, body, flags);
            }
        }

        if (impl_->runtime.redis && !room_to_leave.empty()) {
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
                    auto rid = ensure_room_id_ci(room_to_leave);
                    room_uuid = rid;
                    if (!rid.empty()) {
                        impl_->runtime.redis->srem(make_presence_key("presence:room:", rid), uid);
                        impl_->runtime.redis->srem("room:users:" + room_to_leave, sender_name);
                    }
                } else {
                    room_uuid = ensure_room_id_ci(room_to_leave);
                    impl_->runtime.redis->srem("room:users:" + room_to_leave, sender_name);
                }

                if (room_to_leave != "lobby") {
                    std::size_t remaining = 1;
                    (void)impl_->runtime.redis->scard("room:users:" + room_to_leave, remaining);
                    if (remaining == 0) {
                        impl_->runtime.redis->srem("rooms:active", room_to_leave);
                        impl_->runtime.redis->del("room:password:" + room_to_leave);
                        if (impl_->runtime.db_pool && !room_uuid.empty()) {
                            try {
                                auto uow = impl_->runtime.db_pool->make_repository_unit_of_work();
                                uow->rooms().close(room_uuid);
                                uow->commit();
                                {
                                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                                    impl_->state.room_ids.erase(room_to_leave);
                                    impl_->state.room_owners.erase(room_to_leave);
                                    impl_->state.room_invites.erase(room_to_leave);
                                }
                            } catch (const std::exception& e) {
                                corelog::error("failed to close room: " + std::string(e.what()));
                            }
                        }
                    }
                }

                impl_->runtime.redis->sadd("room:users:lobby", sender_name);
            } catch (...) {}
        }

        std::vector<std::shared_ptr<Session>> lobby_targets;
        std::vector<std::uint8_t> lobby_body;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            lobby_targets = collect_room_targets_locked(impl_->state, "lobby", sender_name);
        }

        server::wire::v1::ChatBroadcast lobby_pb;
        lobby_pb.set_room("lobby");
        lobby_pb.set_sender("(system)");
        lobby_pb.set_text(sender_name + " 님이 입장했습니다");
        lobby_pb.set_sender_sid(0);
        const auto lobby_now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        lobby_pb.set_ts_ms(static_cast<std::uint64_t>(lobby_now64));
        std::string lobby_bytes;
        lobby_pb.SerializeToString(&lobby_bytes);
        lobby_body.assign(lobby_bytes.begin(), lobby_bytes.end());
        for (auto& target : lobby_targets) {
            target->async_send(game_proto::MSG_CHAT_BROADCAST, lobby_body, 0);
        }

        broadcast_refresh("lobby");
        if (!room_to_leave.empty() && room_to_leave != "lobby") {
            broadcast_refresh(room_to_leave);
        }

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
                if (!rid.empty()) {
                    room_id_opt = rid;
                }
            }
            std::vector<std::pair<std::string, std::string>> wb_fields;
            wb_fields.emplace_back("room", room_to_leave);
            wb_fields.emplace_back("user_name", sender_name);
            wb_fields.emplace_back("next_room", next_room);
            emit_write_behind_event("room_leave", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
        }

        send_snapshot(*session_sp, next_room);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_leave dropped: job queue full");
    }
}

} // namespace server::app::chat
