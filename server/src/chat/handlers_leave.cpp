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

// -----------------------------------------------------------------------------
// 방 퇴장 (Leave) 핸들러
// -----------------------------------------------------------------------------
// 사용자가 현재 방에서 나갈 때 호출됩니다.
// 1. 페이로드 파싱 (선택적 방 이름)
// 2. 현재 방 확인 및 퇴장 처리
// 3. 빈 방 정리 (세션이 없으면 방 제거)
// 4. 로비로 이동 처리
// 5. 퇴장 알림 및 로비 입장 알림 브로드캐스트
// 6. Redis Presence 제거 및 Write-behind 이벤트 발행

void ChatService::on_leave(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string room;
    proto::read_lp_utf8(sp, room); // 방 이름은 선택 사항이며 비워두면 현재 방 기준으로 처리한다.

    auto session_sp = s.shared_from_this();
    // DB, Redis, fanout 처리까지 필요하므로 job_queue에서 비동기로 처리한다.
    job_queue_.Push([this, session_sp, room]() {
        const std::string session_id_str = get_or_create_session_uuid(*session_sp);
        std::string user_uuid;
        std::string room_uuid;
        const std::string next_room = "lobby";
        std::vector<std::shared_ptr<Session>> targets;
        std::vector<std::uint8_t> body;
        std::string room_to_leave;
        std::string sender_name;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            // 인증 여부 확인
            if (!state_.authed.count(session_sp.get())) {
                session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized");
                return;
            }
            // 현재 참여 중인 방 확인
            auto itcr = state_.cur_room.find(session_sp.get());
            if (itcr == state_.cur_room.end()) {
                session_sp->send_error(proto::errc::NO_ROOM, "no current room");
                return;
            }
            // 요청된 방 이름이 있다면 현재 방과 일치하는지 검증
            if (!room.empty() && itcr->second != room) {
                session_sp->send_error(proto::errc::ROOM_MISMATCH, "room mismatch");
                return;
            }
            room_to_leave = itcr->second;
            
            // 방에서 세션 제거
            auto itroom = state_.rooms.find(room_to_leave);
            if (itroom != state_.rooms.end()) {
                itroom->second.erase(session_sp);
                auto it2 = state_.user.find(session_sp.get());
                sender_name = (it2 != state_.user.end()) ? it2->second : std::string("guest");
                if (auto it_uuid = state_.user_uuid.find(session_sp.get()); it_uuid != state_.user_uuid.end()) { user_uuid = it_uuid->second; }
                
                // 퇴장 알림을 보낼 대상(방에 남은 사람들) 수집
                auto itb = state_.rooms.find(room_to_leave);
                if (itb != state_.rooms.end()) {
                    auto& set = itb->second;
                    collect_room_sessions(set, targets);
                    // 방이 비었으면 방 정보와 비밀번호 삭제 (로비 제외)
                    if (set.empty() && room_to_leave != std::string("lobby")) { 
                        state_.rooms.erase(itb); 
                        state_.room_passwords.erase(room_to_leave); 
                    }
                }
            }
            // 사용자를 로비로 이동시킴
            state_.cur_room[session_sp.get()] = std::string("lobby");
            state_.rooms["lobby"].insert(session_sp);
        }

        // 방 퇴장 브로드캐스트 메시지를 구성한다.
        // 해당 방 참여자에게 퇴장 알림을 먼저 팬아웃한다.
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

        // Redis presence SET에서도 사용자를 제거해 TTL 기반 알림과 일치시킨다.
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
                        redis_->srem(make_presence_key("presence:room:", rid), uid);
                    }
                }
            } catch (...) {}
        }

        // 로비 입장 알림 메시지를 전송한다.
        // 로비에 입장했다는 알림을 전체 로비 인원에게 재전송한다.
        std::vector<std::shared_ptr<Session>> t2;
        std::vector<std::uint8_t> body2;
        {
            std::lock_guard<std::mutex> lk(state_.mu);
            auto itb = state_.rooms.find("lobby");
            if (itb != state_.rooms.end()) {
                auto& set = itb->second;
                collect_room_sessions(set, t2);
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

        // Write-behind 이벤트 발행
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
            // DLQ/재처리를 위해 leave 이벤트 메타데이터를 Redis stream에 남긴다.
            emit_write_behind_event("room_leave", session_id_str, uid_opt, room_id_opt, std::move(wb_fields));
        }

        // 마지막으로 로비 상태 스냅샷을 내려 클라이언트 UI를 즉시 업데이트한다.
        send_snapshot(*session_sp, next_room);
    });
}

} // namespace server::app::chat
