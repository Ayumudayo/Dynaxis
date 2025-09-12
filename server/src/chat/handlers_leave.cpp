// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/concurrent/job_queue.hpp"

using namespace server::core;
namespace proto = server::core::protocol;

namespace server::app::chat {

void ChatService::on_leave(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string room;
    proto::read_lp_utf8(sp, room); // 방 이름 파싱은 실패해도 괜찮음

    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, room]() {
        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        std::string room_to_leave;
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
            // 퇴장 방송
            auto itroom = state_.rooms.find(room_to_leave);
            if (itroom != state_.rooms.end()) {
                itroom->second.erase(session_sp);
                std::string sender; 
                auto it2 = state_.user.find(session_sp.get()); 
                sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
                proto::write_lp_utf8(body, room_to_leave); 
                proto::write_lp_utf8(body, std::string("(system)")); 
                proto::write_lp_utf8(body, sender + " 님이 퇴장했습니다");
                { std::size_t off_sid = body.size(); body.resize(off_sid + 4); proto::write_be32(0, body.data() + off_sid); }
                auto itb = state_.rooms.find(room_to_leave);
                if (itb != state_.rooms.end()) {
                    auto& set = itb->second;
                    for (auto wit = set.begin(); wit != set.end(); ) { 
                        if (auto p = wit->lock()) { 
                            targets.emplace_back(std::move(p)); 
                            ++wit; 
                        } else { 
                            wit = set.erase(wit); 
                        } 
                    }
                    if (set.empty() && room_to_leave != "lobby") state_.rooms.erase(itb);
                }
            }
            state_.cur_room[session_sp.get()] = std::string("lobby");
            state_.rooms["lobby"].insert(session_sp);
        }
        for (auto& t : targets) { 
            auto f = (t.get() == session_sp.get()) ? proto::FLAG_SELF : 0; 
            t->async_send(proto::MSG_CHAT_BROADCAST, body, f); 
        }
        // 로비 입장 알림
        std::vector<std::shared_ptr<Session>> t2; 
        std::vector<std::uint8_t> body2;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            std::string sender; 
            auto it2 = state_.user.find(session_sp.get()); 
            sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
            proto::write_lp_utf8(body2, std::string("lobby")); 
            proto::write_lp_utf8(body2, std::string("(system)")); 
            proto::write_lp_utf8(body2, sender + " 님이 입장했습니다");
            { std::size_t off_sid = body2.size(); body2.resize(off_sid + 4); proto::write_be32(0, body2.data() + off_sid); }
            auto itb = state_.rooms.find("lobby"); 
            if (itb != state_.rooms.end()) { 
                auto& set = itb->second; 
                for (auto wit = set.begin(); wit != set.end(); ) { 
                    if (auto p = wit->lock()) { 
                        t2.emplace_back(std::move(p)); 
                        ++wit; 
                    } else { 
                        wit = set.erase(wit); 
                    } 
                } 
            }
        }
        for (auto& t : t2) t->async_send(proto::MSG_CHAT_BROADCAST, body2, 0);
    });
}

} // namespace server::app::chat
