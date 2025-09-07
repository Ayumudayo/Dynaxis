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
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <sstream>

#include <ftxui/screen/terminal.hpp>

#include "client/net_client.hpp"
#include "client/store.hpp"
#include "server/core/protocol_errors.hpp"

using namespace ftxui;
// 모든 네트워킹은 NetClient를 통해 수행한다.

int main() {
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);

    // 상태(임시: 네트워크 미연결, UI만 구성)
    std::atomic<bool> connected{false};
    std::vector<std::string> rooms; // 서버 스냅샷으로 동기화됨
    int rooms_selected = 0;
    std::vector<std::string> users = {"(미접속)"};
    int users_selected = 0;
    std::vector<std::string> logs;
    std::mutex logs_mu;
    std::string input;
    int left_width = 26; // 좌측 패널 너비(←/→ 키로 조절)
    std::string current_room = "lobby";
    std::string preview_room; // 좌측 선택(미리보기)
    std::string username = "guest";
    bool show_help = false; // F1로 토글되는 도움말 오버레이
    std::string pending_join_room;

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
        {
            std::lock_guard<std::mutex> lk(logs_mu);
            logs.emplace_back("[" + now_hms() + "] " + std::move(s));
        }
        request_refresh();
        const size_t kMaxLogs = 1000;
        {
            std::lock_guard<std::mutex> lk(logs_mu);
            if (logs.size() > kMaxLogs) {
                logs.erase(logs.begin(), logs.begin() + (logs.size() - kMaxLogs));
            }
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

    // 우측(로그) 렌더러: DOM만, 포커스 없음
    auto right_renderer = Renderer([&] {
        Elements lines;
        std::vector<std::string> snapshot;
        {
            std::lock_guard<std::mutex> lk(logs_mu);
            snapshot = logs;
        }
        lines.reserve(snapshot.size());
        for (auto& l : snapshot) {
            lines.push_back(paragraph(l));
        }
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

        return vbox({ header, separator(), vbox(std::move(lines)) | vscroll_indicator | yframe }) | flex;
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

    // 상단 상태바(반응형)
    auto status_bar = [&] {
        auto sz = Terminal::Size();
        int w = sz.dimx;
        bool wide = w >= 110;
        bool mid  = w >= 90;
        bool sml  = w >= 70;

        Elements parts;
        parts.push_back(text(wide ? " Knights Client " : (mid ? " Knights " : " KC ")) | bold);
        parts.push_back(separator());

        auto led = text(connected.load() ? "●" : "○") | color(connected.load() ? Color::Green : Color::Red);
        if (sml) {
            parts.push_back(text(" 상태:"));
            parts.push_back(text(connected.load() ? "연결됨" : "미연결") | color(connected.load() ? Color::Green : Color::Red));
        } else {
            parts.push_back(led);
        }

        if (mid) {
            parts.push_back(text("  사용자:"));
            auto user_elem = text(username);
            if (username == "guest") user_elem = user_elem | color(Color::Yellow);
            else user_elem = user_elem | bold;
            parts.push_back(user_elem);
            parts.push_back(text("  방:"));
            parts.push_back(text(current_room) | bold);
        } else if (sml) {
            parts.push_back(text("  "));
            auto combo = username + "@" + current_room;
            auto combo_elem = text(combo);
            if (username == "guest") combo_elem = combo_elem | color(Color::Yellow);
            else combo_elem = combo_elem | bold;
            parts.push_back(combo_elem);
        }

        parts.push_back(filler());
        if (wide) {
            parts.push_back(text(" F1:Help  F2:Rooms  F3:Who  F4:Login  F5:Join  F6:Refresh  q/ESC: 종료  ←/→: 폭 조절  Enter: 보내기 ") | dim);
        } else if (mid) {
            parts.push_back(text(" F1 Help  F2 Rooms  F3 Who  F4 Login  F5 Join  F6 Refresh ") | dim);
        } else if (sml) {
            parts.push_back(text(" F1 Help  F6 Refresh ") | dim);
        }

        return hbox(std::move(parts)) | bgcolor(Color::GrayDark) | color(Color::White);
    };

    // 레이아웃 전체 렌더러(포커스 이동을 위해 빈 우측 더미 포함)
    auto right_dummy = Renderer([] { return text(""); });
    auto top_container = Container::Horizontal({ left_container, right_dummy });
    auto main_container = Container::Vertical({ top_container, input_row });

    // 버튼 렌더러 제거

    // 도움말 패널(오버레이)
    auto help_box = [&] {
        auto lines = vbox({
            text("단축키") | bold,
            separator(),
            text("F1  : 도움말 토글"),
            text("F2  : 방 목록 요청(/rooms)"),
            text("F3  : 현재 방 유저 목록(/who)"),
            text("F4  : 입력창에 '/login ' 미리 채움"),
            text("F5  : 입력창에 '/join ' 미리 채움"),
            text("F6  : 전체 새로고침(방/유저 동기화)"),
            text("Enter: 메시지 전송  q/ESC/Ctrl+C: 종료  ←/→: 좌측 폭 조절"),
        });
        return window(text(" 도움말 "), lines) | size(WIDTH, EQUAL, 56) | size(HEIGHT, GREATER_THAN, 10) | center | bgcolor(Color::Black) | color(Color::White);
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
        if (e == Event::Character('q') || e == Event::Escape || e == Event::CtrlC) {
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
                            pending_join_room = line.substr(6);
                            net.send_join(pending_join_room);
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
        if (e == Event::F2) { // Rooms
            if (connected.load()) { net.send_rooms(current_room); }
            return true;
        }
        if (e == Event::F3) { // Who
            if (connected.load()) { net.send_who(current_room); }
            return true;
        }
        if (e == Event::F6) { // Refresh
            if (connected.load()) { net.send_refresh(current_room); }
            else {
                append_log("미연결 상태입니다.");
            }
            return true;
        }
        if (e == Event::F4) { // Login prefix
            input = "/login ";
            return true;
        }
        if (e == Event::F5) { // Join prefix
            input = "/join ";
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
                std::istringstream iss(rest);
                std::string tok;
                while (iss >> tok) { auto p = tok.find('('); std::string name = (p==std::string::npos)?tok:tok.substr(0,p); if(!name.empty()) new_rooms.push_back(name); }
                if (!new_rooms.empty()) {
                    screen.Post([&, new_rooms=std::move(new_rooms)]() mutable {
                        rooms = std::move(new_rooms);
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
                    { std::lock_guard<std::mutex> lk(logs_mu); logs.clear(); }
                });
            } else if (text.rfind(username + " 님이 퇴장했습니다", 0) == 0) {
                screen.Post([&]{ std::lock_guard<std::mutex> lk(logs_mu); logs.clear(); });
            }
            net.send_refresh(current_room);
        }
        // 일반 메시지 렌더링: sender_sid 우선("me"), 없으면 FLAG_SELF 사용
        bool is_me = (cap_sender_sid && sender_sid && my_sid && sender_sid == my_sid) || ((flags & 0x0100 /*FLAG_SELF*/) != 0);
        append_log("[" + room + "] " + (is_me?"me":sender) + ": " + text);
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
    net.set_on_snapshot([&](std::string snap_room, std::vector<std::string> new_rooms, std::vector<std::string> new_users){
        screen.Post([&, snap_room=std::move(snap_room), new_rooms=std::move(new_rooms), new_users=std::move(new_users)]() mutable {
            if (!new_rooms.empty()) {
                rooms = std::move(new_rooms);
                int idx = 0; for (int i=0;i<(int)rooms.size();++i) if (rooms[i]==snap_room) { idx=i; break; }
                rooms_selected = std::clamp(idx, 0, (int)rooms.size()-1);
            }
            current_room = std::move(snap_room);
            users = std::move(new_users); if (users.empty()) users.push_back("<none>");
        });
        request_refresh();
    });

    // 초기 안내 로그 + 연결 시도
    append_log("[system] FTXUI 클라이언트가 시작되었습니다.");
    append_log("[hint] 명령: /login <name>, /join <room>, /leave, /refresh");
    append_log("[hint] 입력 후 Enter로 전송합니다.");
    if (net.connect("127.0.0.1", 5000)) { connected.store(true); append_log("접속됨: 127.0.0.1:5000"); net.send_login(username, ""); }
    else { append_log("연결 실패"); }

    screen.Loop(app_with_events);

    // 종료 시 정리
    net.close();
    return 0;
}

