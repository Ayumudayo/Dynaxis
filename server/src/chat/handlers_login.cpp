#include "server/chat/chat_service.hpp"
#include "chat_service_state.hpp"
#include "server/core/protocol/opcode_policy.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/version.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// 저장소 연동 헤더
#include "server/storage/connection_pool.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include "server/core/storage/redis/client.hpp"

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

/**
 * @brief 로그인 핸들러 구현입니다.
 *
 * 사용자 식별/중복 검증/기본 로비 입장/감사 이벤트를 한 경로로 묶어,
 * 인증 직후 세션 상태와 영속 상태가 동일하게 맞춰지도록 보장합니다.
 * 로그인 직후 규칙이 여러 함수에 흩어지면 "접속은 됐는데 유저 역참조는 아직 이전 사용자 상태인"
 * 어중간한 순간이 생기기 쉬워 이 파일은 그 정착 순서를 명시적으로 유지합니다.
 */
namespace server::app::chat {

// 로그인은 "인증 성공"보다 "서버가 이 세션을 누구로 간주할 것인가"를 확정하는 단계다.
// 세션 UUID, 사용자 역참조, 로비/continuity 상태, 감사 로그가 여기서 같은 순서로 정착돼야 이후 핸들러가 같은 세계를 본다.

void ChatService::on_login(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string user, token;
    // 페이로드 파싱: 닉네임, 토큰(현재는 미사용)
    if (!proto::read_lp_utf8(sp, user) || !proto::read_lp_utf8(sp, token)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad login payload");
        return;
    }

    std::uint16_t client_proto_major = proto::kProtocolVersionMajor;
    std::uint16_t client_proto_minor = proto::kProtocolVersionMinor;
    if (!sp.empty()) {
        // LOGIN_REQ 뒤에 선택적으로 client protocol version(major/minor)을 붙인다.
        // handshake를 완전히 별도 메시지로 찢지 않은 만큼, 여기서라도 버전 불일치를 초기에 잘라야 뒤 경로가 오해하지 않는다.
        if (sp.size() < 4) {
            s.send_error(proto::errc::INVALID_PAYLOAD, "bad login payload");
            return;
        }
        client_proto_major = proto::read_be16(sp.data());
        client_proto_minor = proto::read_be16(sp.data() + 2);
        sp = sp.subspan(4);
        if (!sp.empty()) {
            s.send_error(proto::errc::INVALID_PAYLOAD, "bad login payload");
            return;
        }
        if (!proto::is_protocol_version_compatible(client_proto_major, client_proto_minor)) {
            s.send_error(proto::errc::UNSUPPORTED_VERSION, "unsupported version");
            return;
        }
    }

    auto session_sp = s.shared_from_this();
    // 로그인은 DB/Redis 작업과 write-behind 이벤트가 함께 수행되므로 `job_queue`로 넘긴다.
    // 메인 I/O 스레드에서 이 작업을 직접 수행하면, 느린 저장소가 전체 로그인 latency를 바로 끌어올릴 수 있다.
    // 더 나쁘게는 accept/read 루프까지 막아 신규 연결 자체가 지연될 수 있다.
    if (!job_queue_.TryPush([this, session_sp, user, token]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string tracked_user_uuid;
        std::string lobby_room_id;
        std::string login_ip = session_sp->remote_ip();
        std::string current_room = "lobby";
        std::string current_world = current_runtime_default_world_id();
        std::string continuity_session_id;
        std::string continuity_resume_token;
        std::uint64_t continuity_expires_unix_ms = 0;
        bool resumed_login = false;
        corelog::info("LOGIN_REQ handling started (worker thread)");

        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            impl_->state.by_session_id[session_sp->session_id()] = session_sp;
        }

        const auto requested_resume_token = extract_resume_token(token);
        const auto continuity_resume = try_resume_continuity_lease(token);
        if (requested_resume_token.has_value() && !continuity_resume.has_value()) {
            session_sp->send_error(proto::errc::UNAUTHORIZED, "invalid resume token");
            return;
        }

        // 게스트 여부를 초기에 확정해 이후 저장소/제재/presence 경로가 같은 기준을 보게 한다.
        const std::string requested_user =
            continuity_resume.has_value() ? continuity_resume->effective_user : user;
        bool guest_mode = continuity_resume.has_value() ? false : (user.empty() || user == "guest");
        std::string new_user = ensure_unique_or_error(*session_sp, requested_user);
        if (new_user.empty()) return; // 중복 등으로 실패 시 종료
        const std::string hwid_hash = hash_hwid_token(
            continuity_resume.has_value() ? std::string_view{} : std::string_view(token));

        if (continuity_resume.has_value()) {
            tracked_user_uuid = continuity_resume->user_id;
            continuity_session_id = continuity_resume->logical_session_id;
            continuity_resume_token = continuity_resume->resume_token;
            continuity_expires_unix_ms = continuity_resume->expires_unix_ms;
            current_world = continuity_resume->world_id.empty()
                ? current_runtime_default_world_id()
                : continuity_resume->world_id;
            current_room = continuity_resume->room.empty() ? std::string("lobby") : continuity_resume->room;
            resumed_login = true;
        }

        if (maybe_handle_login_hook(*session_sp, new_user)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        std::string deny_reason;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);

            if (auto it = impl_->state.banned_users.find(new_user); it != impl_->state.banned_users.end()) {
                if (it->second.expires_at <= now) {
                    impl_->state.banned_users.erase(it);
                } else {
                    deny_reason = it->second.reason.empty() ? "temporarily banned" : it->second.reason;
                }
            }

            if (deny_reason.empty() && !login_ip.empty()) {
                if (auto it = impl_->state.banned_ips.find(login_ip); it != impl_->state.banned_ips.end()) {
                    if (it->second <= now) {
                        impl_->state.banned_ips.erase(it);
                    } else {
                        deny_reason = "temporarily banned (ip)";
                    }
                }
            }

            if (deny_reason.empty() && !hwid_hash.empty()) {
                if (auto it = impl_->state.banned_hwid_hashes.find(hwid_hash); it != impl_->state.banned_hwid_hashes.end()) {
                    if (it->second <= now) {
                        impl_->state.banned_hwid_hashes.erase(it);
                    } else {
                        deny_reason = "temporarily banned (device)";
                    }
                }
            }
        }

        if (!deny_reason.empty()) {
            session_sp->send_error(proto::errc::FORBIDDEN, deny_reason);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            // 동일 세션이 이전에 사용했던 이름/guest 상태를 정리하고 새 사용자 맵을 구성한다.
            // 이 정리를 빼먹으면 같은 세션이 by_user에 두 이름으로 남아 귓속말/중복 로그인 판정이 흔들린다.
            if (auto itold = impl_->state.user.find(session_sp.get()); itold != impl_->state.user.end()) {
                auto itset = impl_->state.by_user.find(itold->second);
                if (itset != impl_->state.by_user.end()) {
                    itset->second.erase(session_sp);
                }
            }
            impl_->state.user[session_sp.get()] = new_user;
            impl_->state.by_user[new_user].insert(session_sp);
            impl_->state.authed.insert(session_sp.get());
            if (guest_mode) {
                impl_->state.guest.insert(session_sp.get());
            } else {
                impl_->state.guest.erase(session_sp.get());
            }
            impl_->state.session_ip[session_sp.get()] = login_ip;
            impl_->state.session_hwid_hash[session_sp.get()] = hwid_hash;
            if (!login_ip.empty()) {
                impl_->state.user_last_ip[new_user] = login_ip;
            }
            if (!hwid_hash.empty()) {
                impl_->state.user_last_hwid_hash[new_user] = hwid_hash;
            }
            if (!tracked_user_uuid.empty()) {
                impl_->state.user_uuid[session_sp.get()] = tracked_user_uuid;
            }
            if (!continuity_session_id.empty()) {
                impl_->state.logical_session_id[session_sp.get()] = continuity_session_id;
                impl_->state.logical_session_expires_unix_ms[session_sp.get()] = continuity_expires_unix_ms;
            }
            impl_->state.cur_world[session_sp.get()] = current_world;
            // 기본적으로 로비에 입장시키고, continuity resume이면 마지막 방을 복원한다.
            // fallback room을 먼저 정하지 않으면 로그인 직후 "현재 방 없음" 상태가 길게 남아 refresh와 제재 경로가 흔들린다.
            std::string room = current_room.empty() ? std::string("lobby") : current_room;
            current_room = room;
            impl_->state.cur_room[session_sp.get()] = room;
            impl_->state.rooms[room].insert(session_sp);
        }

        // 게스트와 로그인 사용자를 모두 UUID로 일관되게 식별하고 IP/로그를 남긴다.
        if (impl_->runtime.db_pool) {
            try {
                // UUID가 없으면 게스트 사용자 레코드를 생성한다.
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    auto it = impl_->state.user_uuid.find(session_sp.get());
                    if (it != impl_->state.user_uuid.end()) uid = it->second;
                }
                if (uid.empty()) {
                    try {
                        // 1. 먼저 이름으로 기존 사용자가 있는지 검색합니다.
                        // 이미 존재하는 사용자라면 새로 만들지 않고 ID를 재사용합니다.
                        {
                            auto uow_find = impl_->runtime.db_pool->make_repository_unit_of_work();
                            auto existing = uow_find->users().find_by_name_ci(new_user, 1);
                            if (!existing.empty()) {
                                uid = existing[0].id;
                            }
                        }
                        
                        // 2. 기존 사용자가 없다면 새로 생성을 시도합니다.
                        // find 후 create는 완전한 원자 연산은 아니므로, 아래 재조회 경로가 함께 필요합니다.
                        if (uid.empty()) {
                            auto uow_create = impl_->runtime.db_pool->make_repository_unit_of_work();
                            try {
                                auto u = uow_create->users().create_guest(new_user);
                                uid = u.id;
                                uow_create->commit();
                            } catch (...) {
                                // 3. 생성에 실패했다면(동시성 문제로 그 사이 누군가 만들었을 수 있음),
                                // 다시 한 번 검색해서 ID를 가져옵니다.
                                // 이 보정이 없으면 같은 닉네임에 대해 요청 타이밍에 따라 로그인 성공/실패가 흔들립니다.
                                auto uow_retry = impl_->runtime.db_pool->make_repository_unit_of_work();
                                auto existing = uow_retry->users().find_by_name_ci(new_user, 1);
                                if (!existing.empty()) {
                                    uid = existing[0].id;
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        corelog::warn(std::string("user get/create failed: ") + e.what());
                    }

                    if (!uid.empty()) {
                        std::lock_guard<std::mutex> lk(impl_->state.mu);
                        impl_->state.user_uuid[session_sp.get()] = uid;
                    } else {
                        corelog::error("Failed to obtain UID for user: " + new_user);
                    }
                }
                // write-behind 이벤트용 user_uuid를 저장한다.
                if (!uid.empty()) { tracked_user_uuid = uid; }
                
                // 게스트 이름 강제 정규화는 제거했다.
                // DB에 기록된 이름과 실제 세션 이름이 어긋나면 감사/재현/운영 문의 대응에서 혼란이 더 커졌기 때문이다.

                // users 테이블에 마지막 로그인 IP와 시각을 기록한다.
                {
                    auto ip = login_ip;
                    auto uow3 = impl_->runtime.db_pool->make_repository_unit_of_work();
                    uow3->users().update_last_login(uid, ip);
                    uow3->commit();
                }
                // 현재 방의 room_id를 확보한 뒤 시스템 메시지로 IP를 남긴다.
                auto rid = ensure_room_id_ci(current_room);
                if (!rid.empty()) {
                    lobby_room_id = rid; // write-behind 이벤트에 활용한다.
                    auto ip = login_ip;
                    auto uow2 = impl_->runtime.db_pool->make_repository_unit_of_work();
                    std::string sys = std::string("(login ip=") + ip + ")";
                    (void)uow2->messages().create(rid, current_room, std::nullopt, sys);
                    uow2->commit();
                }
            } catch (const std::exception& e) {
                corelog::warn(std::string("login audit persist failed: ") + e.what());
            }
        }

        if (continuity_session_id.empty() && !tracked_user_uuid.empty()) {
            if (auto lease = issue_continuity_lease(
                    tracked_user_uuid,
                    new_user,
                    current_world,
                    current_room,
                    login_ip.empty() ? std::optional<std::string>{} : std::optional<std::string>{login_ip});
                lease.has_value()) {
                continuity_session_id = lease->logical_session_id;
                continuity_resume_token = lease->resume_token;
                continuity_expires_unix_ms = lease->expires_unix_ms;
                current_world = lease->world_id;
                current_room = lease->room;
                std::lock_guard<std::mutex> lk(impl_->state.mu);
                impl_->state.logical_session_id[session_sp.get()] = continuity_session_id;
                impl_->state.logical_session_expires_unix_ms[session_sp.get()] = continuity_expires_unix_ms;
                impl_->state.cur_world[session_sp.get()] = current_world;
            }
        } else if (!continuity_session_id.empty()) {
            persist_continuity_world(continuity_session_id, current_world, continuity_expires_unix_ms);
            persist_continuity_world_owner(current_world, impl_->continuity.current_owner_id, continuity_expires_unix_ms);
            persist_continuity_room(continuity_session_id, current_room, continuity_expires_unix_ms);
        }

        server::wire::v1::LoginRes pb;
        pb.set_effective_user(new_user);
        pb.set_session_id(session_sp->session_id());
        pb.set_message(resumed_login ? "resumed" : "ok");
        pb.set_is_admin(impl_->runtime.admin_users.count(new_user) > 0);
        pb.set_resumed(resumed_login);
        if (!continuity_session_id.empty()) {
            pb.set_logical_session_id(continuity_session_id);
        }
        if (!continuity_resume_token.empty()) {
            pb.set_resume_token(continuity_resume_token);
        }
        if (continuity_expires_unix_ms > 0) {
            pb.set_resume_expires_unix_ms(continuity_expires_unix_ms);
        }
        if (!current_world.empty()) {
            pb.set_world_id(current_world);
        }
        std::string bytes;
        pb.SerializeToString(&bytes);
        std::vector<std::uint8_t> res(bytes.begin(), bytes.end());

        session_sp->set_session_status(server::core::protocol::SessionStatus::kAuthenticated);
        session_sp->async_send(game_proto::MSG_LOGIN_RES, res, 0);
        
        // presence:user:{uid} 키의 TTL을 갱신해 온라인 리스트를 유지한다.
        // 로그인 직후 이 값을 놓치면 사용자는 실제로 붙어 있는데도 다른 런타임에서는 offline처럼 보일 수 있다.
        if (impl_->runtime.redis) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(impl_->state.mu);
                    auto it = impl_->state.user_uuid.find(session_sp.get());
                    if (it != impl_->state.user_uuid.end()) {
                        uid = it->second;
                    }
                }
                touch_user_presence(uid);
                if (!continuity_session_id.empty()) {
                    persist_continuity_world(continuity_session_id, current_world, continuity_expires_unix_ms);
                    persist_continuity_world_owner(current_world, impl_->continuity.current_owner_id, continuity_expires_unix_ms);
                    persist_continuity_room(continuity_session_id, current_room, continuity_expires_unix_ms);
                }
                
                // 초기 입장 방을 표시용 사용자 목록에 반영한다.
                if (current_room != "lobby") {
                    impl_->runtime.redis->srem("room:users:lobby", new_user);
                    impl_->runtime.redis->sadd("rooms:active", current_room);
                }
                impl_->runtime.redis->sadd("room:users:" + current_room, new_user);
                
                // 초기 입장 방에 있는 다른 유저들에게 갱신 알림 전송
                // 로그인 직후 refresh를 보내 두어야 분산 환경에서도 사용자 목록이 빨리 수렴한다.
                broadcast_refresh(current_room);
            } catch (...) {}
        }

        // 로그인 이벤트를 stream에 남겨 추후 감사, 분석, 재처리 파이프라인과 동기화한다.
        std::optional<std::string> uid_opt;
        if (!tracked_user_uuid.empty()) {
            uid_opt = tracked_user_uuid;
        }
        std::optional<std::string> lobby_id_opt;
        if (!lobby_room_id.empty()) {
            lobby_id_opt = lobby_room_id;
        }
        std::vector<std::pair<std::string, std::string>> wb_fields;
        wb_fields.emplace_back("room", current_room);
        wb_fields.emplace_back("world_id", current_world);
        wb_fields.emplace_back("user_name", new_user);
        if (resumed_login) {
            wb_fields.emplace_back("resumed", "1");
        }
        if (!login_ip.empty()) {
            wb_fields.emplace_back("ip", login_ip);
        }
        // 로그인 과정도 stream에 남겨 이후 감사/재처리 파이프라인과 동기화한다.
        emit_write_behind_event("session_login", session_id_str, uid_opt, lobby_id_opt, std::move(wb_fields));
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_login dropped: job queue full");
    }
}

} // namespace server::app::chat



