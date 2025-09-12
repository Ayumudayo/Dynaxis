// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "wire.pb.h"
#include "server/core/concurrent/job_queue.hpp"

using namespace server::core;
namespace proto = server::core::protocol;

namespace server::app::chat {

void ChatService::on_session_close(std::shared_ptr<Session> s) {
    job_queue_.Push([this, s]() {
        std::vector<std::shared_ptr<Session>> targets; 
        std::vector<std::uint8_t> body; 
        std::string name;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (auto itname = state_.user.find(s.get()); itname != state_.user.end()) {
                name = itname->second; 
            } else {
                name = "guest";
            }
            state_.authed.erase(s.get());
            if (!name.empty()) { 
                auto itset = state_.by_user.find(name); 
                if (itset != state_.by_user.end()) { 
                    itset->second.erase(s); 
                } 
            }
            state_.user.erase(s.get());
            auto itcr = state_.cur_room.find(s.get());
            if (itcr != state_.cur_room.end()) {
                auto room = itcr->second; 
                auto itroom = state_.rooms.find(room);
                if (itroom != state_.rooms.end()) {
                    itroom->second.erase(s);
                    server::wire::v1::ChatBroadcast pb; 
                    pb.set_room(room); 
                    pb.set_sender("(system)"); 
                    pb.set_text(name + " 님이 퇴장했습니다"); 
                    pb.set_sender_sid(0);
                    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
                    pb.set_ts_ms(static_cast<std::uint64_t>(now64));
                    std::string bytes; 
                    pb.SerializeToString(&bytes); 
                    body.assign(bytes.begin(), bytes.end());
                    auto itb = state_.rooms.find(room);
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
                        if (set.empty() && room != std::string("lobby")) state_.rooms.erase(itb);
                    }
                }
                state_.cur_room.erase(itcr);
            }
        }
        for (auto& t : targets) { 
            t->async_send(proto::MSG_CHAT_BROADCAST, body, 0); 
        }
    });
}

} // namespace server::app::chat
