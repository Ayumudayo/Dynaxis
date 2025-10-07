// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// storage
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"
#include <algorithm>
#include <cstdlib>
#include <optional>
#include "server/storage/redis/client.hpp"

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
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string tracked_user_uuid;
        std::string lobby_room_id;
        std::string login_ip = session_sp->remote_ip();
        corelog::info("LOGIN_REQ handling started (worker thread)");
        bool guest_mode = (user.empty() || user == "guest");
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
            if (guest_mode) {
                state_.guest.insert(session_sp.get());
            } else {
                state_.guest.erase(session_sp.get());
            }
            std::string room = "lobby";
            state_.cur_room[session_sp.get()] = room;
            state_.rooms[room].insert(session_sp);
        }

        // 게스트/로그인 사용자 모두에 대해 UUID 부여 및 IP 기록(최소 경로)
        if (db_pool_) {
            try {
                // UUID가 없으면 게스트 유저 생성
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                if (uid.empty()) {
                    auto uow = db_pool_->make_unit_of_work();
                    auto u = uow->users().create_guest(new_user);
                    uid = u.id;
                    uow->commit();
                    std::lock_guard<std::mutex> lk(state_.mu);
                    state_.user_uuid[session_sp.get()] = uid;
                }
                // 추적용 user_uuid 세팅(Write-behind 이벤트에 반영)
                if (!uid.empty()) { tracked_user_uuid = uid; }
                // 게스트 모드라면 닉네임을 UUID 앞 8자로 정규화
                if (guest_mode && !uid.empty()) {
                    std::string uuid8 = uid;
                    uuid8.erase(std::remove(uuid8.begin(), uuid8.end(), '-'), uuid8.end());
                    if (uuid8.size() > 8) uuid8.resize(8);
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto itname = state_.user.find(session_sp.get());
                    std::string prev = (itname != state_.user.end()) ? itname->second : std::string();
                    if (!prev.empty() && prev != uuid8) {
                        auto itset = state_.by_user.find(prev);
                        if (itset != state_.by_user.end()) itset->second.erase(session_sp);
                    }
                    state_.user[session_sp.get()] = uuid8;
                    state_.by_user[uuid8].insert(session_sp);
                    new_user = uuid8;
                }

                // users 테이블에 마지막 로그인 IP/시각 기록
                {
                    auto ip = login_ip;
                    auto uow3 = db_pool_->make_unit_of_work();
                    uow3->users().update_last_login(uid, ip);
                    uow3->commit();
                }
                // 현재 룸의 room_id 확보 후 시스템 메시지로 IP 기록
                auto rid = ensure_room_id_ci("lobby");
                if (!rid.empty()) {
                    lobby_room_id = rid; // write-behind 필드에 사용
                    auto ip = login_ip;
                    auto uow2 = db_pool_->make_unit_of_work();
                    std::string sys = std::string("(login ip=") + ip + ")";
                    (void)uow2->messages().create(rid, std::string("lobby"), std::nullopt, sys);
                    uow2->commit();
                }
            } catch (const std::exception& e) {
                corelog::warn(std::string("login audit persist failed: ") + e.what());
            }
        }
        server::wire::v1::LoginRes pb;
        pb.set_effective_user(new_user);
        pb.set_session_id(session_sp->session_id());
        pb.set_message("ok");
        std::string bytes; pb.SerializeToString(&bytes);
        std::vector<std::uint8_t> res(bytes.begin(), bytes.end());
        session_sp->async_send(proto::MSG_LOGIN_RES, res, 0);

        // 프레즌스(user) TTL 유지: presence:user:{user_id} = 1, TTL
        if (redis_) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    unsigned int ttl = 30; // 기본 30초
                    if (const char* v = std::getenv("PRESENCE_TTL_SEC")) {
                        unsigned long t = std::strtoul(v, nullptr, 10);
                        if (t > 0 && t < 3600) ttl = static_cast<unsigned int>(t);
                    }
                    std::string pfx; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) pfx = p;
                    std::string key = pfx + std::string("presence:user:") + uid;
                    redis_->setex(key, "1", ttl);
                }
            } catch (...) {}
        }
        std::optional<std::string> uid_opt;
        if (!tracked_user_uuid.empty()) {
            uid_opt = tracked_user_uuid;
        }
        std::optional<std::string> lobby_id_opt;
        if (!lobby_room_id.empty()) {
            lobby_id_opt = lobby_room_id;
        }
        std::vector<std::pair<std::string, std::string>> wb_fields;
        wb_fields.emplace_back("room", "lobby");
        wb_fields.emplace_back("user_name", new_user);
        if (!login_ip.empty()) {
            wb_fields.emplace_back("ip", login_ip);
        }
        emit_write_behind_event("session_login", session_id_str, uid_opt, lobby_id_opt, std::move(wb_fields));
    });
}

} // namespace server::app::chat



