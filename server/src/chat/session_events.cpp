// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "wire.pb.h"
#include <cstdlib>
#include "server/storage/redis/client.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include <optional>

using namespace server::core;
namespace proto = server::core::protocol;

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
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (auto itname = state_.user.find(s.get()); itname != state_.user.end()) {
                name = itname->second;
            } else {
                name = "guest";
            }
            state_.authed.erase(s.get());
            if (auto it_uuid = state_.user_uuid.find(s.get()); it_uuid != state_.user_uuid.end()) { user_uuid = it_uuid->second; }
            if (!name.empty()) {
                auto itset = state_.by_user.find(name);
                if (itset != state_.by_user.end()) { itset->second.erase(s); }
            }
            state_.user.erase(s.get());
            // 세션 UUID 정리
            state_.session_uuid.erase(s.get());
            auto itcr = state_.cur_room.find(s.get());
            if (itcr != state_.cur_room.end()) {
                room_left = itcr->second;
                auto itroom = state_.rooms.find(room_left);
                if (itroom != state_.rooms.end()) {
                    itroom->second.erase(s);
                    server::wire::v1::ChatBroadcast pb;
                    pb.set_room(room_left);
                    pb.set_sender("(system)");
                    pb.set_text(name + std::string(" 님이 떠났습니다"));
                    pb.set_sender_sid(0);
                    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    pb.set_ts_ms(static_cast<std::uint64_t>(now64));
                    std::string bytes; pb.SerializeToString(&bytes);
                    body.assign(bytes.begin(), bytes.end());
                    auto itb = state_.rooms.find(room_left);
                    if (itb != state_.rooms.end()) {
                        auto& set = itb->second;
                        for (auto wit = set.begin(); wit != set.end(); ) {
                            if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                            else { wit = set.erase(wit); }
                        }
                        if (set.empty() && room_left != std::string("lobby")) state_.rooms.erase(itb);
                    }
                }
                state_.cur_room.erase(itcr);
            }
        }
        for (auto& t : targets) { t->async_send(proto::MSG_CHAT_BROADCAST, body, 0); }
        // Redis presence: 세션 종료 시 방 SET에서 제거
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
                        std::string pfx; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) pfx = p;
                        std::string pkey = pfx + std::string("presence:room:") + rid;
                        redis_->srem(pkey, uid);
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
        }
    });
}

} // namespace server::app::chat
