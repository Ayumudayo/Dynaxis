#include "client/app/network_router.hpp"

#include "client/app/app_state.hpp"
#include "client/net_client.hpp"

#include <ftxui/component/screen_interactive.hpp>

#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"

#include <sstream>
#include <fstream>
#include <string>
#include <utility>

namespace client::app {

NetworkRouter::NetworkRouter(AppState& state,
                             ::NetClient& net,
                             ftxui::ScreenInteractive& screen,
                             RefreshRequest request_refresh,
                             LogSink log_sink)
    : state_(state),
      net_(net),
      screen_(screen),
      request_refresh_(std::move(request_refresh)),
      log_sink_(std::move(log_sink)) {}

// -----------------------------------------------------------------------------
// 네트워크 콜백 초기화
// -----------------------------------------------------------------------------
// NetClient의 각종 이벤트 콜백을 등록합니다.
// 모든 콜백은 UI 스레드(ScreenInteractive::Post)에서 실행되도록 하여
// 멀티스레드 환경에서의 UI 갱신 안전성을 보장합니다.
void NetworkRouter::Initialize() {
    // 서버 HELLO는 capability 정보를 담고 있으므로 저장 후 로그로 남긴다.
    net_.set_on_hello([this](std::uint16_t caps) {
        screen_.Post([this, caps]() {
            const bool sender_sid = (caps & 0x0002) != 0;
            state_.set_sender_sid_supported(sender_sid);
            log_sink_(std::string("서버 HELLO 수신") + (sender_sid ? " (cap:sender_sid)" : ""));
            request_refresh_();
        });
    });

    // 서버 ERR 응답은 프로토콜 코드별로 힌트를 붙여 로그에 표시한다.
    net_.set_on_err([this](std::uint16_t code, std::string msg) {
        screen_.Post([this, code, msg = std::move(msg)]() mutable {
            std::string hint;
            using namespace server::core::protocol::errc;
            if (code == UNAUTHORIZED) {
                hint = " (로그인이 필요하거나 게스트 채팅이 제한됨)";
            } else if (code == NO_ROOM) {
                hint = " (대상 방이 존재하지 않습니다)";
            } else if (code == NOT_MEMBER) {
                hint = " (해당 방의 구성원이 아닙니다)";
            }
            log_sink_("[ERR " + std::to_string(code) + "] " + msg + hint);
            request_refresh_();
        });
    });

    // 연결 종료 시 상태를 초기화하고 이유를 출력한다.
    net_.set_on_disconnected([this](std::string reason) {
        screen_.Post([this, reason = std::move(reason)]() mutable {
            state_.set_connected(false);
            log_sink_(std::string("[warn] 연결 종료: ") + reason);
            request_refresh_();
        });
    });

    // 로그인 응답이 오면 username/sid를 갱신하고 즉시 스냅샷을 요청한다.
    net_.set_on_login_res([this](std::string effective_user, std::uint32_t sid) {
        screen_.Post([this, effective_user = std::move(effective_user), sid]() mutable {
            if (!effective_user.empty()) {
                state_.set_username(std::move(effective_user));
            }
            state_.set_session_id(sid);
            log_sink_(std::string("로그인 성공") + (sid ? " (sid=" + std::to_string(sid) + ")" : ""));
            net_.send_refresh(state_.current_room());
            request_refresh_();
        });
    });

    // 일반 채팅 브로드캐스트는 system rooms 메시지인지 검사한 뒤 로그에 출력한다.
    net_.set_on_broadcast([this](std::string room,
                                 std::string sender,
                                 std::string text,
                                 std::uint16_t flags,
                                 std::uint32_t sender_sid) {
        // 시스템 메시지(방 목록 갱신 등) 처리
        if (room == "(system)" && sender == "server" && text.rfind("rooms:", 0) == 0) {
            HandleSystemRoomsBroadcast(text.substr(6));
            return;
        }

        screen_.Post([this,
                      room = std::move(room),
                      sender = std::move(sender),
                      text = std::move(text),
                      flags,
                      sender_sid]() mutable {
            bool is_me = false;
            // sender_sid가 지원되면 ID로 비교, 아니면 이름으로 비교 (단, 이름은 중복 가능성 있음)
            if (state_.sender_sid_supported() && sender_sid && state_.session_id() &&
                sender_sid == state_.session_id()) {
                is_me = true;
            }
            if (flags & server::core::protocol::FLAG_SELF) {
                is_me = true;
            }
            auto prefix = "[" + room + "] " + (is_me ? "me" : sender) + ": ";
            log_sink_(prefix + text);

            // Debug logging to file
            std::ofstream debug_log("client_debug.log", std::ios::app);
            if (debug_log) {
                debug_log << prefix << text << std::endl;
            }

            request_refresh_();
        });
    });

    // 귓속말은 방향에 따라 prefix를 달리하고 로그에 기록한다.
    net_.set_on_whisper([this](std::string sender,
                               std::string recipient,
                               std::string text,
                               bool outgoing) {
        screen_.Post([this,
                      sender = std::move(sender),
                      recipient = std::move(recipient),
                      text = std::move(text),
                      outgoing]() mutable {
            std::string prefix = outgoing ? ("[whisper to " + recipient + "] ")
                                          : ("[whisper from " + sender + "] ");
            log_sink_(prefix + text);
            request_refresh_();
        });
    });

    // 실패한 귓속말 결과만 사용자에게 경고한다.
    net_.set_on_whisper_result([this](bool ok, std::string reason) {
        if (ok || reason.empty()) {
            return;
        }
        screen_.Post([this, reason = std::move(reason)]() mutable {
            log_sink_("[warn] 귓속말 전송 실패: " + reason);
            request_refresh_();
        });
    });

    // 현재/프리뷰 방과 일치하는 사용자 목록만 업데이트한다.
    net_.set_on_room_users([this](std::string room, std::vector<std::string> list) {
        screen_.Post([this,
                      room = std::move(room),
                      list = std::move(list)]() mutable {
            if (room == state_.current_room() || room == state_.preview_room()) {
                state_.update_users(std::move(list));
                request_refresh_();
            }
        });
    });

    // 스냅샷에는 방 목록/잠금 상태/현재 방 사용자 등이 포함된다.
    net_.set_on_snapshot([this](std::string snap_room,
                                std::vector<std::string> new_rooms,
                                std::vector<std::string> new_users,
                                std::vector<bool> new_locked) {
        screen_.Post([this,
                      snap_room = std::move(snap_room),
                      new_rooms = std::move(new_rooms),
                      new_users = std::move(new_users),
                      new_locked = std::move(new_locked)]() mutable {
            const bool room_changed = (snap_room != state_.current_room());
            
            // 방 목록 업데이트
            if (!new_rooms.empty()) {
                state_.update_rooms(std::move(new_rooms), std::move(new_locked), snap_room);
            } else if (!new_locked.empty()) {
                // 방 목록은 그대로인데 잠금 상태만 바뀐 경우
                state_.rooms_locked() = std::move(new_locked);
                if (state_.rooms_locked().size() != state_.rooms().size()) {
                    state_.rooms_locked().resize(state_.rooms().size(), false);
                }
            }
            
            state_.set_current_room(std::move(snap_room));
            state_.set_preview_room(state_.current_room());
            state_.update_users(std::move(new_users));
            
            // 방이 바뀌었으면 로그를 초기화하고 자동 스크롤 활성화
            if (room_changed) {
                state_.clear_logs();
                state_.set_log_auto_scroll(true);
            }
            
            // 입장 대기 중이던 방에 들어왔는지 확인
            if (!state_.pending_join_room().empty() &&
                state_.pending_join_room() == state_.current_room()) {
                log_sink_("방 \"" + state_.current_room() + "\"에 입장했습니다.");
                state_.clear_pending_join_room();
            }
            request_refresh_();
        });
    });
}

// -----------------------------------------------------------------------------
// 시스템 방 목록 브로드캐스트 처리
// -----------------------------------------------------------------------------
// 서버가 주기적으로 보내는 방 목록 텍스트를 파싱하여 UI 상태를 갱신합니다.
// 포맷 예: "Lobby(L) Room1 Room2(L)"
void NetworkRouter::HandleSystemRoomsBroadcast(const std::string& payload) {
    std::istringstream iss(payload);
    std::string token;
    std::vector<std::string> rooms;
    std::vector<bool> locks;

    while (iss >> token) {
        auto paren = token.find('(');
        if (paren != std::string::npos) {
            token = token.substr(0, paren);
        }
        bool locked = false;
        const std::string lock_prefix = AppState::kLockIcon;
        if (token.rfind(lock_prefix, 0) == 0) {
            token.erase(0, lock_prefix.size());
            locked = true;
        }
        if (!token.empty()) {
            rooms.push_back(token);
            locks.push_back(locked);
        }
    }

    if (rooms.empty()) {
        return;
    }

    screen_.Post([this,
                  rooms = std::move(rooms),
                  locks = std::move(locks)]() mutable {
        state_.update_rooms(std::move(rooms), std::move(locks), state_.current_room());
        request_refresh_();
    });
}

void NetworkRouter::HandleSystemRoomNotice(const std::string&, const std::string&) {
    // Reserved for future structured system messages.
}

} // namespace client::app
