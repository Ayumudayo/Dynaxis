// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
#include <cstdlib>
#include <optional>
#include "server/storage/redis/client.hpp"

using namespace server::core;
namespace proto = server::core::protocol;

namespace server::app::chat {

void ChatService::on_leave(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string room;
    proto::read_lp_utf8(sp, room); // 방 이름 파싱은 실패해도 진행

    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, room]() {
        const std::string session_id_str = std::to_string(session_sp->session_id());
        std::string user_uuid;
        std::string room_uuid;
        const std::string next_room = "lobby";
        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        std::string room_to_leave;
        std::string sender_name;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(session_sp.get())) {
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized");
                return;
            }
            auto itcr = state_.cur_room.find(session_sp.get());
            if (itcr == state_.cur_room.end()) {
                session_sp->send_error(proto::errc::NO_ROOM, "no current room");
                return;
            }
            if (!room.empty() && itcr->second != room) {
                session_sp->send_error(proto::errc::ROOM_MISMATCH, "room mismatch");
                return;
            }
            room_to_leave = itcr->second;
            auto itroom = state_.rooms.find(room_to_leave);
            if (itroom != state_.rooms.end()) {
                itroom->second.erase(session_sp);
                auto it2 = state_.user.find(session_sp.get());
                sender_name = (it2 != state_.user.end()) ? it2->second : std::string("guest");
                if (auto it_uuid = state_.user_uuid.find(session_sp.get()); it_uuid != state_.user_uuid.end()) { user_uuid = it_uuid->second; }
                auto itb = state_.rooms.find(room_to_leave);
                if (itb != state_.rooms.end()) {
                    auto& set = itb->second;
                    for (auto wit = set.begin(); wit != set.end(); ) {
                        if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                        else { wit = set.erase(wit); }
                    }
                    if (set.empty() && room_to_leave != std::string("lobby")) state_.rooms.erase(itb);
                }
            }
            state_.cur_room[session_sp.get()] = std::string("lobby");
            state_.rooms["lobby"].insert(session_sp);
        }
        // 방 퇴장 브로드캐스트(Protobuf)
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
                t->async_send(proto::MSG_CHAT_BROADCAST, body, f);
            }
        }
        // Redis presence: 방 SET에서 제거
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
                        std::string pfx; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) pfx = p;
                        std::string pkey = pfx + std::string("presence:room:") + rid;
                        redis_->srem(pkey, uid);
                    }
                }
            } catch (...) {}
        }
        // 로비 입장 알림(Protobuf)
        std::vector<std::shared_ptr<Session>> t2;
        std::vector<std::uint8_t> body2;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto itb = state_.rooms.find("lobby");
            if (itb != state_.rooms.end()) {
                auto& set = itb->second;
                for (auto wit = set.begin(); wit != set.end(); ) {
                    if (auto p = wit->lock()) { t2.emplace_back(std::move(p)); ++wit; }
                    else { wit = set.erase(wit); }
                }
            }
        }
        server::wire::v1::ChatBroadcast pb2;
        pb2.set_room("lobby");
        pb2.set_sender("(system)");
        pb2.set_text(sender_name + " 님이 입장했습니다");
        pb2.set_sender_sid(0);
        auto now64_2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        pb2.set_ts_ms(static_cast<std::uint64_t>(now64_2));
        {
            std::string bytes2; pb2.SerializeToString(&bytes2);
            body2.assign(bytes2.begin(), bytes2.end());
        }
        for (auto& t : t2) t->async_send(proto::MSG_CHAT_BROADCAST, body2, 0);
        if (!room_to_leave.empty()) {
            std::optional<std::string> uid_opt;
            if (!user_uuid.empty()) {
                uid_opt = user_uuid;
            }
            std::optional<std::string> room_id_opt;
            if (!room_uuid.empty()) {
                room_id_opt = room_uuid;
            }
            std::vector<std::pair<std::string, std::string>> wb_fields;
            wb_fields.emplace_back("room", room_to_leave);
            wb_fields.emplace_back("user_name", sender_name);
            wb_fields.emplace_back("next_room", next_room);
            emit_write_behind_event("room_leave", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
        }
    });
}

} // namespace server::app::chat
