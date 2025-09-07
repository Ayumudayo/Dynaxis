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

#include <boost/asio.hpp>
#include "server/core/protocol/frame.hpp"
#include "server/core/protocol.hpp"
#include "server/core/protocol_flags.hpp"
#include "server/core/protocol_errors.hpp"

using namespace ftxui;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace proto = server::core::protocol;

static void send_frame_simple(asio::ip::tcp::socket& sock, std::uint16_t msg_id, std::uint16_t flags, const std::vector<std::uint8_t>& payload, std::uint32_t& tx_seq, std::mutex* send_mu = nullptr) {
    proto::FrameHeader h{};
    h.length = static_cast<std::uint16_t>(payload.size());
    h.msg_id = msg_id;
    h.flags  = flags;
    h.seq    = tx_seq++;
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    h.utc_ts_ms32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    std::vector<std::uint8_t> buf;
    buf.resize(proto::k_header_bytes + payload.size());
    proto::encode_header(h, buf.data());
    if (!payload.empty()) std::memcpy(buf.data() + proto::k_header_bytes, payload.data(), payload.size());
    if (send_mu) { std::scoped_lock lk(*send_mu); asio::write(sock, asio::buffer(buf)); }
    else { asio::write(sock, asio::buffer(buf)); }
}

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

    // 네트워크 리소스
    asio::io_context io;
    tcp::resolver resolver(io);
    tcp::socket sock(io);
    std::mutex send_mu;
    std::thread recv_thread;
    std::thread ping_thread;
    std::atomic<bool> running{false};
    std::uint32_t seq = 1;
    std::uint32_t my_sid = 0;
    bool cap_sender_sid = false;

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
            if (connected.load()) {
                std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, preview_room); proto::write_lp_utf8(payload, std::string("/who ") + preview_room);
                send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, payload, seq, &send_mu);
            }
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
    auto send_btn = Button("보내기", [&] {
        if (!input.empty()) {
            std::string line = input; input.clear();
            if (!line.empty() && line[0] == '/') {
                // 슬래시 명령은 Enter 경로에서 처리
                input = line;
            } else {
                if (username == "guest") { append_log("[warn] 게스트는 채팅을 보낼 수 없습니다. /login 으로 로그인하세요."); return; }
                std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, current_room); proto::write_lp_utf8(payload, line);
                send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, payload, seq, &send_mu);
                // 로컬 에코 생략 → 서버 브로드캐스트 사용
            }
        }
    });
    // 액션 버튼 제거(단축키 유지)

    auto input_row = Container::Horizontal({ input_comp, send_btn });

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
        return hbox({ text("> "), input_comp->Render() | flex, text(" "), send_btn->Render() });
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
                            std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, username); proto::write_lp_utf8(payload, std::string());
                            send_frame_simple(sock, proto::MSG_LOGIN_REQ, 0, payload, seq, &send_mu);
                        } else if (line.rfind("/join ", 0) == 0) {
                            pending_join_room = line.substr(6);
                            std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, pending_join_room);
                            send_frame_simple(sock, proto::MSG_JOIN_ROOM, 0, payload, seq, &send_mu);
                        } else if (line.rfind("/leave", 0) == 0) {
                            std::string room = current_room; if (line.size() > 6) { auto pos = line.find_first_not_of(' ', 6); if (pos != std::string::npos) room = line.substr(pos); }
                            std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, room);
                            send_frame_simple(sock, proto::MSG_LEAVE_ROOM, 0, payload, seq, &send_mu);
                        } else if (line == "/refresh") {
                            if (connected.load()) {
                                std::vector<std::uint8_t> p; proto::write_lp_utf8(p, current_room); proto::write_lp_utf8(p, std::string("/refresh"));
                                send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, p, seq, &send_mu);
                            } else {
                                append_log("미연결 상태입니다.");
                            }
                        } else {
                            append_log("알 수 없는 명령: " + line);
                        }
                    } else {
                        // 일반 채팅: 현재 방으로 전송
                        if (username == "guest") { append_log("[warn] 게스트는 채팅을 보낼 수 없습니다. /login 으로 로그인하세요."); return true; }
                        std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, current_room); proto::write_lp_utf8(payload, line);
                        send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, payload, seq, &send_mu);
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
            if (connected.load()) {
                std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, current_room); proto::write_lp_utf8(payload, std::string("/rooms"));
                send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, payload, seq, &send_mu);
            }
            return true;
        }
        if (e == Event::F3) { // Who
            if (connected.load()) {
                std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, current_room); proto::write_lp_utf8(payload, std::string("/who ") + current_room);
                send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, payload, seq, &send_mu);
            }
            return true;
        }
        if (e == Event::F6) { // Refresh
            if (connected.load()) {
                std::vector<std::uint8_t> p; proto::write_lp_utf8(p, current_room); proto::write_lp_utf8(p, std::string("/refresh"));
                send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, p, seq, &send_mu);
            } else {
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

    // 연결 루틴 함수: localhost:5000 고정
    auto start_connect = [&](const std::string& nhost, unsigned short nport){
        // 기존 스레드 종료
        running.store(false);
        try { sock.close(); } catch (...) {}
        if (recv_thread.joinable()) recv_thread.join();
        if (ping_thread.joinable()) ping_thread.join();
        seq = 1;
        try {
            auto endpoints = resolver.resolve(nhost, std::to_string(nport));
            tcp::socket newsock(io);
            asio::connect(newsock, endpoints);
            newsock.set_option(tcp::no_delay(true));
            sock = std::move(newsock);
            connected.store(true);
            running.store(true);
            append_log("접속됨: " + nhost + ":" + std::to_string(nport));
            // 수신 스레드
            recv_thread = std::thread([&]{
                try {
                    while (running.load()) {
                        std::vector<std::uint8_t> hdr(proto::k_header_bytes);
                        asio::read(sock, asio::buffer(hdr));
                        proto::FrameHeader hh{}; proto::decode_header(hdr.data(), hh);
                        std::vector<std::uint8_t> body(hh.length);
                        if (hh.length) asio::read(sock, asio::buffer(body));
                        std::span<const std::uint8_t> in(body.data(), body.size());
                        if (hh.msg_id == proto::MSG_HELLO) {
                            if (body.size() >= 12) {
                                std::uint16_t caps = proto::read_be16(body.data() + 4);
                                cap_sender_sid = (caps & proto::CAP_SENDER_SID) != 0;
                            }
                            append_log(std::string("서버 HELLO 수신") + (cap_sender_sid?" (cap:sender_sid)":""));
                        } else if (hh.msg_id == proto::MSG_PING) {
                            std::vector<std::uint8_t> empty; send_frame_simple(sock, proto::MSG_PONG, 0, empty, seq, &send_mu);
                        } else if (hh.msg_id == proto::MSG_PONG) {
                            // ignore
                        } else if (hh.msg_id == proto::MSG_ERR) {
                            std::uint16_t code = 0, len = 0;
                            std::string msg;
                            if (in.size() >= 4) {
                                code = proto::read_be16(in.data());
                                len  = proto::read_be16(in.data() + 2);
                                in = in.subspan(4);
                                if (in.size() >= len) {
                                    msg.assign(reinterpret_cast<const char*>(in.data()), len);
                                } else {
                                    msg = "(truncated)";
                                }
                            }
                            std::string hint;
                            if (code == server::core::protocol::errc::UNAUTHORIZED) {
                                hint = " (권한 없음: /login 필요 또는 게스트 채팅 불가)";
                            } else if (code == server::core::protocol::errc::NO_ROOM) {
                                hint = " (현재 방 없음)";
                            } else if (code == server::core::protocol::errc::NOT_MEMBER) {
                                hint = " (해당 방 멤버가 아님)";
                            }
                            append_log("[ERR " + std::to_string(code) + "] " + msg + hint);
                        } else if (hh.msg_id == proto::MSG_STATE_SNAPSHOT) {
                            // payload: lp_utf8 current_room, be16 rooms_count, [lp_utf8 room, be16 count]*, be16 users_count, [lp_utf8 user]*
                            std::string snap_room;
                            if (!proto::read_lp_utf8(in, snap_room)) continue;
                            if (in.size() < 2) continue;
                            std::uint16_t rooms_count = proto::read_be16(in.data()); in = in.subspan(2);
                            std::vector<std::string> new_rooms; new_rooms.reserve(rooms_count);
                            for (std::uint16_t i = 0; i < rooms_count; ++i) {
                                std::string rn; if (!proto::read_lp_utf8(in, rn)) { new_rooms.clear(); break; }
                                if (in.size() < 2) { new_rooms.clear(); break; }
                                // count skip
                                in = in.subspan(2);
                                new_rooms.push_back(std::move(rn));
                            }
                            if (in.size() < 2) continue;
                            std::uint16_t users_count = proto::read_be16(in.data()); in = in.subspan(2);
                            std::vector<std::string> new_users; new_users.reserve(users_count);
                            for (std::uint16_t i = 0; i < users_count; ++i) { std::string un; if (!proto::read_lp_utf8(in, un)) { new_users.clear(); break; } new_users.push_back(std::move(un)); }
                            screen.Post([&, snap_room=std::move(snap_room), new_rooms=std::move(new_rooms), new_users=std::move(new_users)]() mutable {
                                if (!new_rooms.empty()) {
                                    rooms = std::move(new_rooms);
                                    int idx = 0; for (int i = 0; i < (int)rooms.size(); ++i) if (rooms[i] == snap_room) { idx = i; break; }
                                    rooms_selected = std::clamp(idx, 0, (int)rooms.size()-1);
                                }
                                current_room = std::move(snap_room);
                                users = std::move(new_users); if (users.empty()) users.push_back("<none>");
                            });
                        } else if (hh.msg_id == proto::MSG_LOGIN_RES) {
                            // payload: u8 status, lp_utf8 msg, [optional lp_utf8 effective_user], [optional u32 sid]
                            std::span<const std::uint8_t> in2(body.data(), body.size());
                            if (in2.size() >= 1) in2 = in2.subspan(1); // status
                            std::string msg_ok; proto::read_lp_utf8(in2, msg_ok);
                            // try read effective username
                            std::string effective;
                            if (in2.size() >= 2) {
                                auto tmp = in2; std::string t;
                                if (proto::read_lp_utf8(tmp, t)) { effective = std::move(t); in2 = tmp; }
                            }
                            if (!effective.empty()) { username = effective; }
                            if (in2.size() >= 4) my_sid = proto::read_be32(in2.data());
                            append_log("로그인 응답 수신" + std::string(my_sid?" (sid="+std::to_string(my_sid)+")":""));
                            if (connected.load()) {
                                std::vector<std::uint8_t> p; proto::write_lp_utf8(p, current_room); proto::write_lp_utf8(p, std::string("/refresh"));
                                send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, p, seq, &send_mu);
                            }
                        } else if (hh.msg_id == proto::MSG_CHAT_BROADCAST) {
                            std::string room; std::string sender; std::string text;
                            bool ok = proto::read_lp_utf8(in, room) && proto::read_lp_utf8(in, sender) && proto::read_lp_utf8(in, text);
                            if (!ok) { append_log("[WARN] CHAT_BROADCAST payload 파싱 실패"); continue; }
                            // optional: sender_sid(u32) + ts(u64)
                            std::uint32_t sender_sid = 0;
                            if (in.size() >= 4) { sender_sid = proto::read_be32(in.data()); in = in.subspan(4); }
                            // (system)/server에서 내려주는 구조화 응답 파싱
                            if (room == "(system)" && sender == "server") {
                                if (text.rfind("rooms:", 0) == 0) {
                                    // 예: "rooms: lobby(3) games(1) ..."
                                    std::string rest = text.substr(6);
                                    std::vector<std::string> new_rooms;
                                    // 구분자는 공백
                                    std::istringstream iss(rest);
                                    std::string tok;
                                    while (iss >> tok) {
                                        // name(count) -> name
                                        auto p = tok.find('(');
                                        std::string name = (p==std::string::npos) ? tok : tok.substr(0, p);
                                        if (!name.empty()) new_rooms.push_back(name);
                                    }
                                    if (!new_rooms.empty()) {
                                        screen.Post([&, new_rooms=std::move(new_rooms)]() mutable {
                                            rooms = std::move(new_rooms);
                                            // 선택 갱신: 현재 방 우선
                                            int idx = 0;
                                            for (int i = 0; i < (int)rooms.size(); ++i) if (rooms[i] == current_room) { idx = i; break; }
                                            rooms_selected = std::clamp(idx, 0, (int)rooms.size()-1);
                                        });
                                    }
                                    continue;
                                }
                                // who() 텍스트 응답 경로는 제거(구버전 호환 미사용)
                            }
                            // 입장/퇴장 시스템 메시지 수신 시 스냅샷 요청 및 전환 반영
                            if (sender == "(system)" && (text.find("입장했습니다") != std::string::npos || text.find("퇴장했습니다") != std::string::npos)) {
                                if (text.rfind(username + " 님이 입장했습니다", 0) == 0) {
                                    // 나의 입장 확인 → 현재 방 전환 + 로그 초기화
                                    current_room = room;
                                    int idx = 0; for (int i = 0; i < (int)rooms.size(); ++i) if (rooms[i] == current_room) { idx = i; break; }
                                    rooms_selected = std::clamp(idx, 0, (int)rooms.size()-1);
                                    screen.Post([&]{ std::lock_guard<std::mutex> lk(logs_mu); logs.clear(); });
                                } else if (text.rfind(username + " 님이 퇴장했습니다", 0) == 0) {
                                    screen.Post([&]{ std::lock_guard<std::mutex> lk(logs_mu); logs.clear(); });
                                }
                                if (connected.load()) {
                                    std::vector<std::uint8_t> p; proto::write_lp_utf8(p, current_room); proto::write_lp_utf8(p, std::string("/refresh"));
                                    send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, p, seq, &send_mu);
                                }
                            }
                            // 일반 채팅/시스템 메시지 렌더: capability가 있으면 sender_sid 우선, 없으면 플래그
                            std::string display_sender = (cap_sender_sid && sender_sid && my_sid && sender_sid == my_sid)
                                ? std::string("me")
                                : ((hh.flags & proto::FLAG_SELF) ? std::string("me") : sender);
                            append_log("[" + room + "] " + display_sender + ": " + text);
                        } else if (hh.msg_id == proto::MSG_ROOM_USERS) {
                            std::string room; if (!proto::read_lp_utf8(in, room)) continue;
                            if (in.size() < 2) continue; std::uint16_t n = proto::read_be16(in.data()); in = in.subspan(2);
                            std::vector<std::string> list; list.reserve(n);
                            for (std::uint16_t i=0;i<n;++i){ std::string u; if (!proto::read_lp_utf8(in, u)) { list.clear(); break; } list.push_back(std::move(u)); }
                            screen.Post([&, room=std::move(room), list=std::move(list)]() mutable {
                                if (room == current_room || room == preview_room) {
                                    users = std::move(list);
                                    if (users.empty()) users.push_back("<none>");
                                    users_selected = std::clamp(users_selected, 0, (int)users.size()-1);
                                }
                            });
                        } else {
                            append_log("알 수 없는 메시지: id=" + std::to_string(hh.msg_id));
                        }
                    }
                } catch (...) {
                    // 소켓 종료/에러
                    connected.store(false);
                    running.store(false);
                    append_log("연결 종료");
                }
            });
            // 연결 직후 자동 로그인(guest)
            {
                std::vector<std::uint8_t> payload; proto::write_lp_utf8(payload, username); proto::write_lp_utf8(payload, std::string());
                send_frame_simple(sock, proto::MSG_LOGIN_REQ, 0, payload, seq, &send_mu);
            }

            // 핑 스레드
            ping_thread = std::thread([&]{
                try {
                    while (running.load()) {
                        std::this_thread::sleep_for(std::chrono::seconds(8));
                        if (!running.load()) break;
                        std::vector<std::uint8_t> empty;
                        send_frame_simple(sock, proto::MSG_PING, 0, empty, seq, &send_mu);
                    }
                } catch (...) { }
            });
        } catch (const std::exception& ex) {
            append_log(std::string("연결 실패: ") + ex.what());
            connected.store(false);
            running.store(false);
        }
    };

    // 초기 안내 로그 + 자동 연결 시도
    append_log("[system] FTXUI 클라이언트가 시작되었습니다.");
    append_log("[hint] 명령: /login <name>, /join <room>, /leave, /refresh");
    append_log("[hint] 입력 후 Enter로 전송합니다.");
    // 비동기 연결 시작(화면 렌더링과 병행)
    std::thread auto_conn([&]{ start_connect("127.0.0.1", 5000); });
    auto_conn.detach();

    screen.Loop(app_with_events);

    // 종료 시 정리
    running.store(false);
    try { sock.close(); } catch (...) {}
    if (recv_thread.joinable()) recv_thread.join();
    if (ping_thread.joinable()) ping_thread.join();
    return 0;
}

