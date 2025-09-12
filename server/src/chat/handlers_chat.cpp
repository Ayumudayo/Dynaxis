// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

void ChatService::on_chat_send(Session& s, std::span<const std::uint8_t> payload) {
    std::string room, text;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    if (!proto::read_lp_utf8(sp, room) || !proto::read_lp_utf8(sp, text)) { 
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad chat payload"); 
        return; 
    }

    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, room, text]() {
        corelog::info(std::string("CHAT_SEND: room=") + (room.empty()?"(empty)":room) + ", text=" + text);
        // /refresh는 인증 없이도 허용(스냅샷 반환)
        if (text == "/refresh") {
            std::string current;
            {
                std::lock_guard<std::mutex> lk(state_.mu); 
                auto itcr = state_.cur_room.find(session_sp.get()); 
                current = (itcr != state_.cur_room.end()) ? itcr->second : std::string("lobby");
            }
            send_snapshot(*session_sp, current); 
            return;
        }
        // 인증 체크
        std::string current_room = room;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }
            if (current_room.empty()) {
                auto it = state_.cur_room.find(session_sp.get()); 
                if (it == state_.cur_room.end()) { 
                    session_sp->send_error(proto::errc::NO_ROOM, "no current room"); 
                    return; 
                } 
                current_room = it->second;
            }
        }
        // 슬래시 명령
        if (!text.empty() && text[0] == '/') {
            if (text == "/rooms") { send_rooms_list(*session_sp); return; }
            if (text.rfind("/who", 0) == 0) {
                std::string target = current_room; 
                if (text.size() > 4) { 
                    auto pos = text.find_first_not_of(' ', 4); 
                    if (pos != std::string::npos) target = text.substr(pos); 
                }
                send_room_users(*session_sp, target); 
                return;
            }
            if (text.rfind("/whisper ", 0) == 0) {
                std::string rest = text.substr(9); auto spc = rest.find(' ');
                if (spc == std::string::npos) {
                    std::vector<std::uint8_t> body; 
                    proto::write_lp_utf8(body, std::string("(system)")); 
                    proto::write_lp_utf8(body, std::string("server")); 
                    proto::write_lp_utf8(body, std::string("usage: /whisper <user> <message>"));
                    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
                    std::size_t off = body.size(); 
                    body.resize(off + 8); 
                    std::uint64_t ts = static_cast<std::uint64_t>(now64); 
                    for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; } 
                    session_sp->async_send(proto::MSG_CHAT_BROADCAST, body, 0); 
                    return;
                }
                std::string target_user = rest.substr(0, spc); 
                std::string wtext = rest.substr(spc + 1);
                std::vector<std::shared_ptr<Session>> targets;
                {
                    std::lock_guard<std::mutex> lk(state_.mu); 
                    auto itset = state_.by_user.find(target_user); 
                    if (itset != state_.by_user.end()) { 
                        for (auto wit = itset->second.begin(); wit != itset->second.end(); ) { 
                            if (auto p = wit->lock()) { 
                                targets.emplace_back(std::move(p)); 
                                ++wit; 
                            } else { 
                                wit = itset->second.erase(wit); 
                            } 
                        } 
                    }
                }
                std::string sender; 
                { 
                    std::lock_guard<std::mutex> lk(state_.mu); 
                    auto it2 = state_.user.find(session_sp.get()); 
                    sender = (it2 != state_.user.end()) ? it2->second : std::string("guest"); 
                }
                server::wire::v1::ChatBroadcast pb; 
                pb.set_room("(direct)"); 
                pb.set_sender(sender); 
                pb.set_text(std::string("(to ") + target_user + ") " + wtext); 
                pb.set_sender_sid(session_sp->session_id());
                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
                pb.set_ts_ms(static_cast<std::uint64_t>(now64));
                std::string bytes; pb.SerializeToString(&bytes);
                std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
                targets.emplace_back(session_sp); 
                for (auto& t : targets) t->async_send(proto::MSG_CHAT_BROADCAST, body, 0); 
                return;
            }
        }
        // 일반 채팅
        std::vector<std::shared_ptr<Session>> targets;
        std::string sender;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it2 = state_.user.find(session_sp.get()); 
            sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
            if (sender == "guest") { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "guest cannot chat"); 
                return; 
            }
            auto it = state_.rooms.find(current_room); 
            if (it != state_.rooms.end()) { 
                auto& set = it->second; 
                for (auto wit = set.begin(); wit != set.end(); ) { 
                    if (auto p = wit->lock()) { 
                        targets.emplace_back(std::move(p)); 
                        ++wit; 
                    } else { 
                        wit = set.erase(wit); 
                    } 
                } 
            }
        }
        server::wire::v1::ChatBroadcast pb; 
        pb.set_room(current_room); 
        pb.set_sender(sender); 
        pb.set_text(text); 
        pb.set_sender_sid(session_sp->session_id());
        auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
        pb.set_ts_ms(static_cast<std::uint64_t>(now64));
        std::string bytes; pb.SerializeToString(&bytes);
        if (targets.empty()) { 
            session_sp->async_send(proto::MSG_CHAT_BROADCAST, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), proto::FLAG_SELF); 
        } else { 
            for (auto& t : targets) { 
                auto f = (t.get() == session_sp.get()) ? proto::FLAG_SELF : 0; 
                t->async_send(proto::MSG_CHAT_BROADCAST, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), f); 
            } 
        }
    });
}

} // namespace server::app::chat
