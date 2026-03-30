
#include "server/chat/chat_service.hpp"
#include "chat_message_delivery.hpp"
#include "chat_spam_moderation.hpp"
#include "chat_service_state.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
// 저장소 연동 헤더
#include "server/storage/connection_pool.hpp"
#include <vector>
#include <chrono>

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

/**
 * @brief 채팅/귓속말 핸들러 구현입니다.
 *
 * slash command, 권한 검사, 브로드캐스트, write-behind 발행을
 * 작업 큐에서 순차 처리해 데이터 경합과 I/O 스레드 블로킹을 줄입니다.
 */
namespace server::app::chat {

// -----------------------------------------------------------------------------
// 귓속말(Whisper) 처리 핸들러
// -----------------------------------------------------------------------------
// 귓속말은 단순 브로드캐스트보다 상태 조회가 많다.
// 상대 사용자 위치, 차단 상태, 현재 세션 유효성을 함께 봐야 하므로 별도 경로로 다룬다.

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
    // whisper는 타 세션 조회와 저장소 접근이 섞이므로 job_queue로 넘긴다.
    // I/O 스레드에서 직접 처리하면 느린 조회 하나가 다른 클라이언트의 패킷 수신까지 막을 수 있다.
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
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            if (!impl_->state.authed.count(session_sp.get())) { 
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized"); 
                return; 
            }
            auto it = impl_->state.cur_room.find(session_sp.get()); 
            if (it == impl_->state.cur_room.end()) { 
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

        if (try_handle_slash_command(session_sp, current_room, text)) {
            return;
        }

        auto moderation = ChatSendModerationResult{};
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            moderation = evaluate_chat_send_moderation_locked(impl_->state, impl_->runtime, session_sp);
            if (moderation.sender == "guest") {
                session_sp->send_error(proto::errc::UNAUTHORIZED, "guest cannot chat");
                return;
            }
        }

        if (moderation.sender_is_banned) {
            send_system_notice(*session_sp, moderation.moderation_reason);
            session_sp->send_error(proto::errc::FORBIDDEN, moderation.moderation_reason);
            session_sp->stop();
            return;
        }
        if (moderation.sender_is_muted) {
            send_system_notice(*session_sp, moderation.moderation_reason);
            session_sp->send_error(proto::errc::FORBIDDEN, moderation.moderation_reason);
            return;
        }
        if (moderation.spam_escalated) {
            send_system_notice(*session_sp, moderation.moderation_reason);
            session_sp->send_error(proto::errc::FORBIDDEN, moderation.moderation_reason);
            if (moderation.spam_escalated_to_ban) {
                for (auto& penalized : moderation.penalized_sessions) {
                    send_system_notice(*penalized, moderation.moderation_reason);
                    penalized->stop();
                }
            }
            return;
        }

        // 플러그인은 내장 slash command 다음, 실제 fan-out 직전에 탄다.
        // 그래야 기본 명령 체계는 유지하면서도 마지막 정책 게이트로 moderation/변환을 덧댈 수 있다.
        if (maybe_handle_chat_hook_plugin(*session_sp, current_room, moderation.sender, text)) {
            return;
        }

        dispatch_room_message(session_sp, current_room, moderation.sender, text);
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
        std::lock_guard<std::mutex> lk(impl_->state.mu);
        auto itcr = impl_->state.cur_room.find(session_sp.get());
        current = (itcr != impl_->state.cur_room.end()) ? itcr->second : std::string("lobby");
    }

    // 현재 방의 상태 스냅샷 전송
    send_snapshot(*session_sp, current);

    if (!impl_->runtime.db_pool) {
        return;
    }
    // DB에 마지막 읽은 위치 갱신 (현재 방의 최신 메시지로)
    try {
        std::string uid;
        {
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            auto it = impl_->state.user_uuid.find(session_sp.get());
            if (it != impl_->state.user_uuid.end()) uid = it->second;
        }
        if (uid.empty()) return;
        auto rid = ensure_room_id_ci(current);
        if (rid.empty()) return;
        auto uow = impl_->runtime.db_pool->make_repository_unit_of_work();
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
            std::lock_guard<std::mutex> lk(impl_->state.mu);
            auto it = impl_->state.cur_room.find(session_sp.get());
            target = (it != impl_->state.cur_room.end()) ? it->second : std::string("lobby");
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

