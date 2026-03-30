#include "server/chat/chat_service.hpp"

#include "chat_room_state.hpp"
#include "chat_service_state.hpp"

#include "server/chat/chat_hook_plugin_abi.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/storage/redis/client.hpp"
#include "wire.pb.h"

#include <chrono>
#include <cstdlib>
#include <optional>

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;

namespace server::app::chat {

void ChatService::on_session_close(std::shared_ptr<Session> s) {
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
            if (auto uuid_it = impl_->state.user_uuid.find(s.get()); uuid_it != impl_->state.user_uuid.end()) {
                user_uuid = uuid_it->second;
            }
            if (!name.empty()) {
                auto itset = impl_->state.by_user.find(name);
                if (itset != impl_->state.by_user.end()) {
                    itset->second.erase(s);
                }
            }
            impl_->state.user.erase(s.get());
            impl_->state.session_uuid.erase(s.get());
            impl_->state.logical_session_id.erase(s.get());
            impl_->state.logical_session_expires_unix_ms.erase(s.get());
            impl_->state.cur_world.erase(s.get());
            impl_->state.session_ip.erase(s.get());
            impl_->state.session_hwid_hash.erase(s.get());

            auto current_room_it = impl_->state.cur_room.find(s.get());
            if (current_room_it != impl_->state.cur_room.end()) {
                room_left = current_room_it->second;
                if (room_exists_locked(impl_->state, room_left)) {
                    server::wire::v1::ChatBroadcast pb;
                    pb.set_room(room_left);
                    pb.set_sender("(system)");
                    pb.set_text(name + std::string(" 님이 연결 종료되었습니다"));
                    pb.set_sender_sid(0);
                    const auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    pb.set_ts_ms(static_cast<std::uint64_t>(now64));
                    std::string bytes;
                    pb.SerializeToString(&bytes);
                    body.assign(bytes.begin(), bytes.end());
                    targets = remove_session_from_room_locked(impl_->state, s, room_left, name).targets;
                }
                impl_->state.cur_room.erase(current_room_it);
            }
        }

        for (auto& target : targets) {
            target->async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);
        }

        if (impl_->runtime.redis && !room_left.empty()) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    auto it = impl_->state.user_uuid.find(s.get());
                    if (it != impl_->state.user_uuid.end()) {
                        uid = it->second;
                    }
                }
                if (!uid.empty()) {
                    auto rid = ensure_room_id_ci(room_left);
                    room_uuid = rid;
                    if (!rid.empty()) {
                        impl_->runtime.redis->srem(make_presence_key("presence:room:", rid), uid);
                    }
                }
                impl_->runtime.redis->srem("room:users:" + room_left, name);
                if (room_left != "lobby") {
                    impl_->runtime.redis->sadd("room:users:lobby", name);
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
            emit_write_behind_event("session_close", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
            broadcast_refresh(room_left);
        }
    });
}

} // namespace server::app::chat
