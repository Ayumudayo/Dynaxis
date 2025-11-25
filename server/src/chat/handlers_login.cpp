#include "server/chat/chat_service.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// 저장소 연동 헤더
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

// -----------------------------------------------------------------------------
// 로그인 처리 핸들러
// -----------------------------------------------------------------------------
// 사용자의 로그인 요청을 처리합니다.
// 1. 페이로드 파싱 (닉네임, 토큰)
// 2. 세션 UUID 생성 및 할당
// 3. 닉네임 중복 검사 및 게스트 처리
// 4. DB에 사용자 정보(게스트 포함) 등록 및 로그인 기록
// 5. 로비(Lobby) 입장 처리
// 6. Write-behind 이벤트 발행 (로그인 감사 로그)

void ChatService::on_login(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string user, token;
    // 페이로드 파싱: 닉네임, 토큰(현재는 미사용)
    if (!proto::read_lp_utf8(sp, user) || !proto::read_lp_utf8(sp, token)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad login payload");
        return;
    }

    auto session_sp = s.shared_from_this();
    // 로그인은 DB/Redis 작업과 write-behind 이벤트가 함께 수행되므로 job_queue로 넘긴다.
    // I/O 바운드 작업이 많으므로 비동기 처리가 필수적입니다.
    job_queue_.Push([this, session_sp, user, token]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string tracked_user_uuid;
        std::string lobby_room_id;
        std::string login_ip = session_sp->remote_ip();
        corelog::info("LOGIN_REQ handling started (worker thread)");
        
        // 게스트 모드 판별: 닉네임이 없거나 "guest"인 경우
        bool guest_mode = (user.empty() || user == "guest");
        std::string new_user = ensure_unique_or_error(*session_sp, user);
        if (new_user.empty()) return; // 중복 등으로 실패 시 종료

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            // 동일 세션이 이전에 사용했던 이름/guest 상태를 정리하고 새 사용자 맵을 구성한다.
            // 재로그인 시 이전 정보를 정리합니다.
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
            // 기본적으로 로비에 입장시킵니다.
            std::string room = "lobby";
            state_.cur_room[session_sp.get()] = room;
            state_.rooms[room].insert(session_sp);
        }

        // 게스트와 로그인 사용자를 모두 UUID로 일관되게 식별하고 IP/로그를 남긴다.
        if (db_pool_) {
            try {
                // UUID가 없으면 게스트 사용자 레코드를 생성한다.
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
                // write-behind 이벤트용 user_uuid를 저장한다.
                if (!uid.empty()) { tracked_user_uuid = uid; }
                
                // 게스트라면 닉네임을 UUID 앞 8자로 정규화한다.
                // 고유한 닉네임을 보장하기 위함입니다.
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

                // users 테이블에 마지막 로그인 IP와 시각을 기록한다.
                {
                    auto ip = login_ip;
                    auto uow3 = db_pool_->make_unit_of_work();
                    uow3->users().update_last_login(uid, ip);
                    uow3->commit();
                }
                // 현재 방의 room_id를 확보한 뒤 시스템 메시지로 IP를 남긴다.
                // 로비 채팅창에 접속 로그를 남깁니다 (관리 목적).
                auto rid = ensure_room_id_ci("lobby");
                if (!rid.empty()) {
                    lobby_room_id = rid; // write-behind 이벤트에 활용한다.
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
        
        // 로그인 응답 전송
        server::wire::v1::LoginRes pb;
        pb.set_effective_user(new_user);
        pb.set_session_id(session_sp->session_id());
        pb.set_message("ok");
        std::string bytes; pb.SerializeToString(&bytes);
        std::vector<std::uint8_t> res(bytes.begin(), bytes.end());
        session_sp->async_send(proto::MSG_LOGIN_RES, res, 0);

        // presence:user:{uid} 키의 TTL을 주기적으로 갱신해 온라인 리스트를 유지한다.
        if (redis_) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                touch_user_presence(uid);
            } catch (...) {}
        }
        
        // Write-behind 이벤트 발행
        // 로그인 이벤트를 스트림에 기록하여 추후 분석이나 알림 등에 활용합니다.
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
        // 로그인 과정도 stream에 남겨 이후 감사/재처리 파이프라인과 동기화한다.
        emit_write_behind_event("session_login", session_id_str, uid_opt, lobby_id_opt, std::move(wb_fields));
    });
}

} // namespace server::app::chat



