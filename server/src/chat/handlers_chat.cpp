
#include "server/chat/chat_service.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// 저장소 연동 헤더
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"
#include "server/storage/redis/client.hpp"
#include <vector>
#include <cstdlib>
#include <atomic>

using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

// -----------------------------------------------------------------------------
// 귓속말(Whisper) 처리 핸들러
// -----------------------------------------------------------------------------
// 특정 사용자에게 1:1 메시지를 전송합니다.
// 대상 사용자가 현재 서버에 접속해 있다면 직접 전송하고,
// 다른 서버에 있다면 Redis Pub/Sub 등을 통해 라우팅해야 할 수도 있습니다 (현재 구현은 로컬 세션 위주).

void ChatService::on_whisper(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string target;
    std::string text;
    // 페이로드 파싱: 대상 닉네임, 메시지 내용
    if (!proto::read_lp_utf8(sp, target) || !proto::read_lp_utf8(sp, text)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad whisper payload");
        return;
    }
    auto session_sp = s.shared_from_this();
    // whisper 처리는 DB 조회/타 세션 접근이 필요하므로 job_queue에서 비동기로 처리한다.
    // 동시성 문제를 피하기 위해 메인 로직은 JobQueue Worker 스레드에서 실행됩니다.
    // 이렇게 하면 I/O 스레드는 즉시 다음 패킷을 받을 준비를 할 수 있습니다 (Non-blocking).
    if (!job_queue_.TryPush([this, session_sp, target = std::move(target), text = std::move(text)]() {
        dispatch_whisper(session_sp, target, text);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_whisper dropped: job queue full");
    }
}

// -----------------------------------------------------------------------------
// 일반 채팅 메시지 처리 핸들러
// -----------------------------------------------------------------------------
// 방에 있는 모든 사용자에게 메시지를 브로드캐스트합니다.
// 1. 권한 검사 (로그인 여부, 현재 방 일치 여부)
// 2. 슬래시 커맨드 처리 (/rooms, /who, /whisper 등)
// 3. 메시지 영속화 (DB 저장)
// 4. Redis Recent History 캐싱
// 5. 로컬 세션 브로드캐스트
// 6. Redis Pub/Sub을 통한 분산 브로드캐스트 (Fan-out)

void ChatService::on_chat_send(Session& s, std::span<const std::uint8_t> payload) {
    std::string room, text;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    // 페이로드 파싱: 방 이름, 메시지 내용
    if (!proto::read_lp_utf8(sp, room) || !proto::read_lp_utf8(sp, text)) { 
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad chat payload"); 
        return; 
    }

    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp, room = std::move(room), text = std::move(text)]() mutable {
        corelog::info(std::string("CHAT_SEND: room=") + (room.empty()?"(empty)":room) + ", text=" + text);
        
        // /refresh는 상태 스냅샷을 강제로 다시 받게 하는 관리 명령이다.
        // 클라이언트 상태가 꼬였을 때 유용합니다.
        // 예: 네트워크 끊김 후 재접속 시 UI 갱신용
        if (text == "/refresh") {
            handle_refresh(session_sp);
            return;
        }

        // 현재 세션이 보고 있는 room 정보와 authoritative room을 비교한다.
        // 클라이언트가 주장하는 방과 서버가 알고 있는 방이 다르면 에러 처리합니다.
        std::string current_room = room;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            if (!state_.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }
            auto it = state_.cur_room.find(session_sp.get()); 
            if (it == state_.cur_room.end()) { 
                session_sp->send_error(proto::errc::NO_ROOM, "no current room"); 
                return; 
            }
            const std::string& authoritative_room = it->second;
            if (!current_room.empty() && current_room != authoritative_room) {
                session_sp->send_error(proto::errc::ROOM_MISMATCH, "room mismatch");
                return;
            }
            current_room = authoritative_room;
        }

        // 슬래시 명령 분기를 처리한다.
        // 채팅 메시지가 '/'로 시작하면 명령어로 간주합니다.
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
            // 귓속말 단축 명령어 지원 (/w, /whisper)
            if (text.rfind("/whisper ", 0) == 0 || text.rfind("/w ", 0) == 0) {
                const bool long_form = text.rfind("/whisper ", 0) == 0;
                std::string args = text.substr(long_form ? 9 : 3);
                auto leading = args.find_first_not_of(' ');
                if (leading != std::string::npos && leading > 0) {
                    args.erase(0, leading);
                }
                auto spc = args.find(' ');
                if (spc == std::string::npos) {
                    send_system_notice(*session_sp, "usage: /whisper <user> <message>");
                    send_whisper_result(*session_sp, false, "invalid payload");
                    return;
                }
                std::string target_user = args.substr(0, spc);
                std::string wtext = args.substr(spc + 1);
                auto msg_leading = wtext.find_first_not_of(' ');
                if (msg_leading != std::string::npos && msg_leading > 0) {
                    wtext.erase(0, msg_leading);
                }
                if (target_user.empty() || wtext.empty()) {
                    send_system_notice(*session_sp, "usage: /whisper <user> <message>");
                    send_whisper_result(*session_sp, false, "invalid payload");
                    return;
                }
                dispatch_whisper(session_sp, target_user, wtext);
                return;
            }
        }

        // Chat hook plugins (optional): custom commands / moderation / transforms.
        // Runs after built-in slash commands, right before broadcasting.
        std::string sender;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it2 = state_.user.find(session_sp.get());
            sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
        }
        if (maybe_handle_chat_hook_plugin(*session_sp, current_room, sender, text)) {
            return;
        }

        // 일반 채널 브로드캐스트 경로.
        std::vector<std::shared_ptr<Session>> targets;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it2 = state_.user.find(session_sp.get()); 
            sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
            // 게스트는 채팅 권한이 없습니다.
            if (sender == "guest") { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "guest cannot chat"); 
                return; 
            }
            auto it = state_.rooms.find(current_room); 
            if (it != state_.rooms.end()) { 
                collect_room_sessions(it->second, targets);
            }
        }

        // Protobuf 메시지 생성
        server::wire::v1::ChatBroadcast pb; 
        pb.set_room(current_room); 
        pb.set_sender(sender); 
        pb.set_text(text); 
        pb.set_sender_sid(session_sp->session_id());
        auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); 
        pb.set_ts_ms(static_cast<std::uint64_t>(now64));
        std::string bytes; pb.SerializeToString(&bytes);

        // 영속화(선택): Postgres에 저장해 Redis recent cache가 비어도 복구되게 한다.
        // DB 저장은 신뢰성을 위해 필수적입니다.
        std::string persisted_room_id;
        std::uint64_t persisted_msg_id = 0;
        if (db_pool_) {
            try {
                persisted_room_id = ensure_room_id_ci(current_room);
                if (!persisted_room_id.empty()) {
                    std::optional<std::string> uid_opt;
                    {
                        std::lock_guard<std::mutex> lk(state_.mu);
                        auto it = state_.user_uuid.find(session_sp.get());
                        if (it != state_.user_uuid.end()) uid_opt = it->second;
                    }
                    auto uow = db_pool_->make_unit_of_work();
                    auto msg = uow->messages().create(persisted_room_id, current_room, uid_opt, text);
                    persisted_msg_id = msg.id;
                    uow->commit();
                }
            } catch (const std::exception& e) {
                corelog::error(std::string("Failed to persist message: ") + e.what());
            }
        }

        // Redis 캐시는 recent history 스펙을 만족하기 위해 DB id와 payload를 그대로 복제한다.
        // 최근 메시지 목록을 빠르게 조회하기 위해 Redis List를 사용합니다.
        if (redis_ && !persisted_room_id.empty() && persisted_msg_id != 0) {
            server::wire::v1::StateSnapshot::SnapshotMessage snapshot_msg;
            snapshot_msg.set_id(persisted_msg_id);
            snapshot_msg.set_sender(sender);
            snapshot_msg.set_text(text);
            snapshot_msg.set_ts_ms(static_cast<std::uint64_t>(now64));
            if (!cache_recent_message(persisted_room_id, snapshot_msg)) {
                corelog::warn(std::string("Redis recent history update failed for room_id=") + persisted_room_id);
            } else {
                // Debug log
                // corelog::info("Cached message " + std::to_string(persisted_msg_id) + " to room " + persisted_room_id);
            }
        } else {
            if (!redis_) corelog::warn("Redis not available for caching");
            if (persisted_room_id.empty()) corelog::warn("Room ID not found for caching");
            if (persisted_msg_id == 0) corelog::warn("Message ID not generated (DB persist failed?)");
        }

        // 로컬 세션들에게 메시지 전송
        if (targets.empty()) { 
            // 방에 혼자 있는 경우 자신에게만 에코
            session_sp->async_send(game_proto::MSG_CHAT_BROADCAST, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), proto::FLAG_SELF); 
        } else { 
            for (auto& t : targets) { 
                auto f = (t.get() == session_sp.get()) ? proto::FLAG_SELF : 0; 
                t->async_send(game_proto::MSG_CHAT_BROADCAST, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), f); 
            } 
        }

        // 사용자 프레즌스 heartbeat TTL을 갱신한다.
        // 채팅 활동이 있으면 온라인 상태로 간주하여 TTL을 연장합니다.
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

        // Redis Pub/Sub 팬아웃(옵션)을 수행한다.
        // 다른 서버 인스턴스에 접속한 사용자들에게도 메시지를 전달하기 위함입니다.
        if (redis_) {
            try {
                if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
                    static std::atomic<std::uint64_t> publish_total{0};
                    std::string channel = presence_.prefix + std::string("fanout:room:") + current_room;
                    std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    std::string message;
                    message.reserve(3 + gateway_id_.size() + payload.size());
                    message.append("gw=").append(gateway_id_);
                    message.push_back('\n');
                    message.append(payload);
                    redis_->publish(channel, std::move(message));
                    auto n = ++publish_total;
                    corelog::info(std::string("metric=publish_total value=") + std::to_string(n) + " room=" + current_room);
                }
            } catch (...) {}
        }

        // 마지막으로 본 메시지 id를 membership.last_seen에 반영한다.
        // 사용자가 어디까지 읽었는지 추적하는 기능입니다 (Read Receipt).
        if (db_pool_ && persisted_msg_id > 0 && !persisted_room_id.empty()) {
            try {
                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto it = state_.user_uuid.find(session_sp.get());
                    if (it != state_.user_uuid.end()) uid = it->second;
                }
                if (!uid.empty()) {
                    auto uow = db_pool_->make_unit_of_work();
                    uow->memberships().update_last_seen(uid, persisted_room_id, persisted_msg_id);
                    uow->commit();
                }
            } catch (...) {}
        }
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_chat_send dropped: job queue full");
    }
}

// -----------------------------------------------------------------------------
// 상태 갱신 (Refresh) 핸들러
// -----------------------------------------------------------------------------
// 클라이언트가 현재 상태(방 정보, 최근 메시지 등)를 다시 요청할 때 사용합니다.
// 주로 네트워크 재연결 후 상태 동기화를 위해 호출됩니다.

void ChatService::handle_refresh(std::shared_ptr<Session> session_sp) {
    std::string current;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itcr = state_.cur_room.find(session_sp.get());
        current = (itcr != state_.cur_room.end()) ? itcr->second : std::string("lobby");
    }

    // 현재 방의 상태 스냅샷 전송
    send_snapshot(*session_sp, current);

    if (!db_pool_) {
        return;
    }
    // DB에 마지막 읽은 위치 갱신 (현재 방의 최신 메시지로)
    try {
        std::string uid;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it = state_.user_uuid.find(session_sp.get());
            if (it != state_.user_uuid.end()) uid = it->second;
        }
        if (uid.empty()) return;
        auto rid = ensure_room_id_ci(current);
        if (rid.empty()) return;
        auto uow = db_pool_->make_unit_of_work();
        auto last_id = uow->messages().get_last_id(rid);
        if (last_id > 0) {
            uow->memberships().update_last_seen(uid, rid, last_id);
            uow->commit();
        }
    } catch (...) {
    }
}

void ChatService::on_rooms_request(Session& s, std::span<const std::uint8_t>) {
    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp]() {
        send_rooms_list(*session_sp);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_rooms_request dropped: job queue full");
    }
}

void ChatService::on_room_users_request(Session& s, std::span<const std::uint8_t> payload) {
    std::string requested;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    proto::read_lp_utf8(sp, requested);
    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp, requested = std::move(requested)]() mutable {
        std::string target = requested;
        if (target.empty()) {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto it = state_.cur_room.find(session_sp.get());
            target = (it != state_.cur_room.end()) ? it->second : std::string("lobby");
        }
        send_room_users(*session_sp, target);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_room_users_request dropped: job queue full");
    }
}

void ChatService::on_refresh_request(Session& s, std::span<const std::uint8_t>) {
    auto session_sp = s.shared_from_this();
    if (!job_queue_.TryPush([this, session_sp]() {
        handle_refresh(session_sp);
    })) {
        session_sp->send_error(proto::errc::SERVER_BUSY, "server busy");
        corelog::warn("on_refresh_request dropped: job queue full");
    }
}

} // namespace server::app::chat

