// UTF-8 인코딩, 한국어 주석
// Knights: FTXUI 기반 최소 클라이언트(처음부터 다시 시작)
// - 레이아웃: 상단 상태바 / 좌측(방/유저) / 우측(로그) / 하단 입력
// - Enter로 입력 전송(로그에 추가), q/ESC/Ctrl+C로 종료

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <sstream>

#include <ftxui/screen/terminal.hpp>

#include "client/net_client.hpp"
#include "client/store.hpp"
#include "client/app/state.hpp"
#include "client/ui/status_bar.hpp"
#include "server/core/protocol/protocol_errors.hpp"

using namespace ftxui;
// 모든 네트워킹은 NetClient를 통해 수행한다.

int main() {
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);
    std::atomic<bool> ui_running{false};

    // 상태(임시: 네트워크 미연결, UI만 구성)
    std::atomic<bool> connected{false};
    std::vector<std::string> rooms; // 서버 스냅샷으로 동기화됨
    std::vector<bool> rooms_locked;
    int rooms_selected = 0;
    std::vector<std::string> users = {"(미접속)"};
    int users_selected = 0;
    std::vector<std::string> logs;
    std::atomic<bool> log_auto_scroll{true};
    int log_selected = 0;
    std::string input;
    int left_width = 26; // 좌측 패널 너비(←/→ 키로 조절)
    std::string current_room = "lobby";
    std::string preview_room; // 좌측 선택(미리보기)
    std::string username = "guest";
    bool show_help = false; // F1로 토글되는 도움말 오버레이
    std::string pending_join_room;
    const std::string lock_icon = "\xF0\x9F\x94\x92";

    // 네트워크 리소스(NetClient 사용)
    std::uint32_t my_sid = 0;
    bool cap_sender_sid = false;
    NetClient net;

    auto request_refresh = [&]{ screen.PostEvent(Event::Custom); };

    auto now_hms = [](){
        using namespace std::chrono;
        auto t = system_clock::to_time_t(system_clock::now());
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    };

    auto append_log = [&](std::string s) {
        if (!s.empty() && s.back() == '\n') s.pop_back();
        std::string line = "[" + now_hms() + "] " + std::move(s);
        auto update = [&, line = std::move(line)]() mutable {
            logs.emplace_back(std::move(line));
            const size_t kMaxLogs = 1000;
            if (logs.size() > kMaxLogs) {
                const auto remove = logs.size() - kMaxLogs;
                logs.erase(logs.begin(), logs.begin() + remove);
                log_selected = std::max(0, log_selected - static_cast<int>(remove));
            }
            if (log_auto_scroll.load()) {
                log_selected = logs.empty() ? 0 : static_cast<int>(logs.size()) - 1;
            } else if (!logs.empty()) {
                log_selected = std::clamp(log_selected, 0, static_cast<int>(logs.size()) - 1);
            } else {
                log_selected = 0;
            }
            request_refresh();
        };
        if (ui_running.load()) {
            screen.Post(std::move(update));
        } else {
            update();
        }
    };
    // 메뉴 옵션: 방 이동 시 서버에 /who 질의(미리보기). current_room은 변경하지 않음
    MenuOption rooms_opt;
    rooms_opt.on_change = [&] {
        if (rooms_selected >= 0 && rooms_selected < (int)rooms.size()) {
            preview_room = rooms[rooms_selected];
            if (connected.load()) { net.send_who(preview_room); }
        }
    };

    rooms_opt.entries_option.transform = [&](const EntryState& state) {
        std::string label = state.label;
        if (state.index >= 0 && state.index < static_cast<int>(rooms_locked.size()) && rooms_locked[state.index]) {
            label = lock_icon + " " + label;
        }
        auto elem = text(label);
        if (state.focused) {
            elem = elem | bold;
        }
        return elem;
    };

    // 컴포넌트 구성
    auto rooms_menu = Menu(&rooms, &rooms_selected, rooms_opt);
    auto users_menu = Menu(&users, &users_selected);

    auto left_container = Container::Vertical({
        rooms_menu,
        users_menu,
    });

    auto input_comp = Input(&input, "메시지 입력...");
    // 액션 버튼 제거(단축키/Enter만 사용)
    auto input_row = Container::Horizontal({ input_comp });

    MenuOption log_menu_option = MenuOption::Vertical();
    log_menu_option.underline.enabled = false;
    log_menu_option.entries_option.transform = [](const EntryState& state) {
        auto elem = paragraph(state.label);
        if (state.focused) {
            elem = elem | bold;
        }
        return elem;
    };
    log_menu_option.on_change = [&] {
        int last_index = logs.empty() ? 0 : static_cast<int>(logs.size()) - 1;
        log_auto_scroll.store(log_selected >= last_index);
    };
    auto log_menu = Menu(&logs, &log_selected, log_menu_option);

    // 우측(로그) 렌더러: DOM만, 포커스 없음
    auto right_renderer = Renderer(log_menu, [&] {
        auto sz2 = Terminal::Size();
        int w2 = sz2.dimx;
        bool mid2 = w2 >= 80;
        bool sml2 = w2 >= 60;
        Elements head;
        head.push_back(text(" 방:") | dim);
        head.push_back(text(" "));
        head.push_back(text(current_room) | bold);
        if (mid2) {
            head.push_back(text("  유저수:") | dim);
            head.push_back(text(std::to_string((int)users.size())));
            head.push_back(text("  사용자:") | dim);
            head.push_back(text(username));
        } else if (sml2) {
            head.push_back(text("  "));
            head.push_back(text(std::to_string((int)users.size()) + "명"));
        }
        head.push_back(filler());
        head.push_back(text(connected.load() ? "●" : "○") | color(connected.load() ? Color::Green : Color::Red));
        auto header = hbox(std::move(head));
        auto log_view = log_menu->Render() | vscroll_indicator | yframe | flex;
        return vbox({ header, separator(), log_view }) | flex;
    });

    // 좌측 렌더러
    auto left_renderer = Renderer(left_container, [&] {
        auto r = window(text("방"), rooms_menu->Render() | vscroll_indicator | yframe);
        auto u = window(text("유저"), users_menu->Render() | vscroll_indicator | yframe);
        return vbox({ r | flex, separator(), u | flex }) | size(WIDTH, EQUAL, left_width);
    });

    // 입력 행 렌더러
    auto input_renderer = Renderer(input_row, [&] {
        return hbox({ text("> "), input_comp->Render() | flex });
    });

    // 상단 상태바(반응형) - 분리된 UI 모듈 사용
    auto status_bar = [&] {
        client::app::State st;
        st.connected.store(connected.load());
        st.rooms = rooms; st.rooms_selected = rooms_selected;
        st.users = users; st.users_selected = users_selected;
        st.current_room = current_room;
        st.username = username;
        return client::ui::RenderStatusBar(st);
    };

    // 레이아웃 전체 렌더러(좌측 목록 + 우측 로그)
    auto top_container = Container::Horizontal({ left_container, right_renderer });
    auto main_container = Container::Vertical({ top_container, input_row });

    // 버튼 렌더러 제거

    // 도움말 패널(오버레이)
    auto help_box = [&] {
        auto lines = vbox({
            text(" 단축키") | bold,
            separator(),
            text(" F1  : 도움말 전환"),
            text(" F5  : 현재 방 새로고침(/refresh)"),
            text(" Enter: 메시지 전송  ESC/Ctrl+C: 종료  ←/→: 좌측 폭 조절"),
            text(" /join <room> [password]"),
            text(" /whisper <user> <message>"),
            text("")
        });
        auto panel = window(text(" 도움말 "), lines)
            | bgcolor(Color::Black)
            | color(Color::White)
            | clear_under;
        return vbox({
            filler(),
            hbox({ filler(), panel, filler() }),
            filler(),
        });
    };

    auto app = Renderer(main_container, [&] {
        auto content = hbox({ left_renderer->Render(), separator(), right_renderer->Render() | flex });
        auto base = vbox({ status_bar(), separator(), content | flex, separator(), input_renderer->Render() });
        if (show_help) {
            return dbox({ base, help_box() });
        }
        return base;
    });

    // 키 이벤트: 종료/Enter/좌우 폭 조절
    auto app_with_events = CatchEvent(app, [&](Event e) {
        if (e == Event::Escape || e == Event::CtrlC) {
            screen.ExitLoopClosure()();
            return true;
        }
        if (e == Event::Return) {
            if (input_comp->Focused()) {
                if (!input.empty()) {
                    std::string line = input; input.clear();
                    // 슬래시 명령 처리
                    if (!line.empty() && line[0] == '/') {
                        if (line.rfind("/login ", 0) == 0) {
                            username = line.substr(7);
                            net.send_login(username, "");
                        } else if (line.rfind("/join ", 0) == 0) {
                            std::string args = line.substr(6);
                            auto pos = args.find(' ');
                            std::string room_arg = args;
                            std::string password_arg;
                            if (pos != std::string::npos) {
                                room_arg = args.substr(0, pos);
                                password_arg = args.substr(pos + 1);
                                auto trim_pos = password_arg.find_first_not_of(' ');
                                if (trim_pos != std::string::npos) password_arg.erase(0, trim_pos);
                                else password_arg.clear();
                            }
                            if (room_arg.empty()) {
                                append_log("usage: /join <room> [password]");
                            } else {
                                pending_join_room = room_arg;
                                net.send_join(room_arg, password_arg);
                            }
                        } else if (line.rfind("/whisper ", 0) == 0 || line.rfind("/w ", 0) == 0) {
                            const bool long_form = line.rfind("/whisper ", 0) == 0;
                            std::string args = line.substr(long_form ? 9 : 3);
                            auto leading = args.find_first_not_of(' ');
                            if (leading == std::string::npos) {
                                args.clear();
                            } else if (leading > 0) {
                                args.erase(0, leading);
                            }
                            auto pos = args.find(' ');
                            if (pos == std::string::npos) {
                                append_log("usage: /whisper <user> <message>");
                            } else if (username == "guest") {
                                append_log("[warn] 로그인 후에만 귓속말을 사용할 수 있습니다.");
                            } else {
                                std::string target = args.substr(0, pos);
                                std::string message = args.substr(pos + 1);
                                auto trim_pos = message.find_first_not_of(' ');
                                if (trim_pos == std::string::npos) {
                                    message.clear();
                                } else if (trim_pos > 0) {
                                    message.erase(0, trim_pos);
                                }
                                if (target.empty() || message.empty()) {
                                    append_log("usage: /whisper <user> <message>");
                                } else if (target == username) {
                                    append_log("[warn] 자기 자신에게는 귓속말을 보낼 수 없습니다.");
                                } else {
                                    net.send_whisper(target, message);
                                }
                            }
                        } else if (line.rfind("/leave", 0) == 0) {
                            std::string room = current_room; if (line.size() > 6) { auto pos = line.find_first_not_of(' ', 6); if (pos != std::string::npos) room = line.substr(pos); }
                            net.send_leave(room);
                        } else if (line == "/refresh") {
                            if (connected.load()) { net.send_refresh(current_room); }
                            else {
                                append_log("미연결 상태입니다.");
                            }
                        } else {
                            append_log("알 수 없는 명령: " + line);
                        }
                    } else {
                        // 일반 채팅: 현재 방으로 전송
                        if (username == "guest") { append_log("[warn] 게스트는 채팅을 보낼 수 없습니다. /login 으로 로그인하세요."); return true; }
                        net.send_chat(current_room, line);
                        // 로컬 에코 생략 → 서버 브로드캐스트 사용
                    }
                }
                return true;
            }
        }
        if (e == Event::F1) { // Help
            show_help = !show_help; request_refresh();
            return true;
        }
        if (e == Event::F5) { // Refresh
            if (connected.load()) { net.send_refresh(current_room); }
            else {
                append_log("연결되지 않았습니다.");
            }
            return true;
        }
        if (e == Event::ArrowLeft) {
            left_width = std::max(16, left_width - 1);
            return true;
        }
        if (e == Event::ArrowRight) {
            left_width = std::min(60, left_width + 1);
            return true;
        }
        return false;
    });

    // NetClient 콜백 등록: 수신 이벤트를 UI 상태로 반영
    net.set_on_hello([&](std::uint16_t caps){
        cap_sender_sid = (caps & 0x0002 /*CAP_SENDER_SID*/) != 0;
        append_log(std::string("서버 HELLO 수신") + (cap_sender_sid?" (cap:sender_sid)":""));
    });
    net.set_on_err([&](std::uint16_t code, std::string msg){
        std::string hint;
        if (code == server::core::protocol::errc::UNAUTHORIZED) hint = " (권한 없음: /login 필요 또는 게스트 채팅 불가)";
        else if (code == server::core::protocol::errc::NO_ROOM) hint = " (현재 방 없음)";
        else if (code == server::core::protocol::errc::NOT_MEMBER) hint = " (해당 방 멤버가 아님)";
        append_log("[ERR " + std::to_string(code) + "] " + msg + hint);
    });
    net.set_on_login_res([&](std::string effective_user, std::uint32_t sid){
        if (!effective_user.empty()) {
            screen.Post([&, effective_user=std::move(effective_user)]() mutable { username = std::move(effective_user); });
        }
        my_sid = sid;
        append_log(std::string("로그인 응답 수신") + (my_sid?" (sid="+std::to_string(my_sid)+")":""));
        net.send_refresh(current_room);
        request_refresh();
    });
    net.set_on_broadcast([&](std::string room, std::string sender, std::string text, std::uint16_t flags, std::uint32_t sender_sid){
        // (system)/server 구조화 응답: rooms
        if (room == "(system)" && sender == "server") {
            if (text.rfind("rooms:", 0) == 0) {
                std::string rest = text.substr(6);
                std::vector<std::string> new_rooms;
                std::vector<bool> new_locks;
                std::istringstream iss(rest);
                std::string tok;
                while (iss >> tok) {
                    auto p = tok.find('(');
                    std::string name = (p==std::string::npos)?tok:tok.substr(0,p);
                    bool locked = false;
                    if (!name.empty() && name.rfind(lock_icon, 0) == 0) {
                        name = name.substr(lock_icon.size());
                        locked = true;
                    }
                    if(!name.empty()) {
                        new_rooms.push_back(name);
                        new_locks.push_back(locked);
                    }
                }
                if (!new_rooms.empty()) {
                    screen.Post([&, new_rooms=std::move(new_rooms), new_locks=std::move(new_locks)]() mutable {
                        rooms = std::move(new_rooms);
                        rooms_locked = std::move(new_locks);
                        if (rooms_locked.size() != rooms.size()) rooms_locked.resize(rooms.size(), false);
                        int idx = 0; for (int i=0;i<(int)rooms.size();++i) if (rooms[i]==current_room) { idx=i; break; }
                        rooms_selected = std::clamp(idx, 0, (int)rooms.size()-1);
                    });
                }
                return;
            }
        }
        // 입장/퇴장 시스템 메시지 시 스냅샷 동기화
        if (sender == "(system)" && (text.find("입장했습니다") != std::string::npos || text.find("퇴장했습니다") != std::string::npos)) {
            if (text.rfind(username + " 님이 입장했습니다", 0) == 0) {
                screen.Post([&, room=std::move(room)]() mutable {
                    current_room = std::move(room);
                    int idx = 0; for (int i=0;i<(int)rooms.size();++i) if (rooms[i]==current_room) { idx=i; break; }
                    rooms_selected = std::clamp(idx, 0, (int)rooms.size()-1);
                    logs.clear();
                    log_selected = 0;
                    log_auto_scroll.store(true);
                    request_refresh();
                });
            } else if (text.rfind(username + " 님이 퇴장했습니다", 0) == 0) {
                screen.Post([&]{
                    logs.clear();
                    log_selected = 0;
                    log_auto_scroll.store(true);
                    request_refresh();
                });
            }

            net.send_refresh(current_room);
        }
        // 일반 메시지 렌더링: sender_sid 우선("me"), 없으면 FLAG_SELF 사용
        bool is_me = (cap_sender_sid && sender_sid && my_sid && sender_sid == my_sid) || ((flags & 0x0100 /*FLAG_SELF*/) != 0);
        append_log("[" + room + "] " + (is_me?"me":sender) + ": " + text);
    });
    net.set_on_whisper([&](std::string sender, std::string recipient, std::string text, bool outgoing){
        std::string prefix = outgoing ? ("[whisper to " + recipient + "] ") : ("[whisper from " + sender + "] ");
        append_log(prefix + text);
    });
    net.set_on_whisper_result([&](bool ok, std::string reason){
        if (!ok && !reason.empty()) append_log("[warn] whisper failed: " + reason);
    });
    net.set_on_room_users([&](std::string room, std::vector<std::string> list){
        screen.Post([&, room=std::move(room), list=std::move(list)]() mutable {
            if (room == current_room || room == preview_room) {
                users = std::move(list);
                if (users.empty()) users.push_back("<none>");
                users_selected = std::clamp(users_selected, 0, (int)users.size()-1);
            }
        });
        request_refresh();
    });
    net.set_on_snapshot([&](std::string snap_room, std::vector<std::string> new_rooms, std::vector<std::string> new_users, std::vector<bool> new_locked){
        screen.Post([&, snap_room=std::move(snap_room), new_rooms=std::move(new_rooms), new_users=std::move(new_users), new_locked=std::move(new_locked)]() mutable {
            if (!new_rooms.empty()) {
                rooms = std::move(new_rooms);
                rooms_locked = std::move(new_locked);
                int idx = 0; for (int i=0;i<(int)rooms.size();++i) if (rooms[i]==snap_room) { idx=i; break; }
                rooms_selected = std::clamp(idx, 0, (int)rooms.size()-1);
            } else {
                rooms_locked = std::move(new_locked);
                if (rooms_locked.size() != rooms.size()) rooms_locked.resize(rooms.size(), false);
            }
            current_room = std::move(snap_room);
            users = std::move(new_users); if (users.empty()) users.push_back("<none>");
        });
        request_refresh();
    });

    // 초기 안내 로그 + 연결 시도
    append_log("[system] FTXUI 클라이언트가 시작되었습니다.");
    append_log("[hint] 명령: /login <name>, /join <room> [password], /whisper <user> <message>, /leave, /refresh");
    append_log("[hint] " + lock_icon + " 표시된 방은 비밀번호가 필요합니다.");
    append_log("[hint] 입력 후 Enter로 전송합니다.");
    if (net.connect("127.0.0.1", 5000)) { connected.store(true); append_log("접속됨: 127.0.0.1:5000"); net.send_login(username, ""); }
    else { append_log("연결 실패"); }

    ui_running.store(true);
    screen.Loop(app_with_events);
    ui_running.store(false);

    // 종료 시 정리
    net.close();
    return 0;
}
