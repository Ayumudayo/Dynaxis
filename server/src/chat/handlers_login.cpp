// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

void ChatService::on_login(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string user, token;
    if (!proto::read_lp_utf8(sp, user) || !proto::read_lp_utf8(sp, token)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad login payload");
        return;
    }

    auto session_sp = s.shared_from_this();
    job_queue_.Push([this, session_sp, user, token]() {
        corelog::info("LOGIN_REQ 처리 시작 (워커 스레드)");
        std::string new_user = ensure_unique_or_error(*session_sp, user);
        if (new_user.empty()) return;

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (auto itold = state_.user.find(session_sp.get()); itold != state_.user.end()) {
                auto itset = state_.by_user.find(itold->second);
                if (itset != state_.by_user.end()) {
                    itset->second.erase(session_sp);
                }
            }
            state_.user[session_sp.get()] = new_user;
            state_.by_user[new_user].insert(session_sp);
            state_.authed.insert(session_sp.get());
            std::string room = "lobby";
            state_.cur_room[session_sp.get()] = room;
            state_.rooms[room].insert(session_sp);
        }
        server::wire::v1::LoginRes pb;
        pb.set_effective_user(new_user);
        pb.set_session_id(session_sp->session_id());
        pb.set_message("ok");
        std::string bytes; pb.SerializeToString(&bytes);
        std::vector<std::uint8_t> res(bytes.begin(), bytes.end());
        session_sp->async_send(proto::MSG_LOGIN_RES, res, 0);
    });
}

} // namespace server::app::chat
