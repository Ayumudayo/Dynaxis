#include "client/ui/status_bar.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

using namespace ftxui;

namespace client::ui {

Element RenderStatusBar(const client::app::AppState& state) {
    const auto size = Terminal::Size();
    const int width = size.dimx;
    const bool wide = width >= 110;
    const bool mid = width >= 90;
    const bool small = width >= 70;

    Elements parts;
    parts.push_back(text(wide ? " Knights Client " : (mid ? " Knights " : " KC ")) | bold);
    parts.push_back(separator());

    const bool connected = state.connected();
    auto led = text(connected ? "●" : "○") | color(connected ? Color::Green : Color::Red);
    if (small) {
        parts.push_back(text(" 상태:"));
        parts.push_back(text(connected ? "연결됨" : "미연결") | color(connected ? Color::Green : Color::Red));
    } else {
        parts.push_back(led);
    }

    const auto& username = state.username();
    const auto& room = state.current_room();

    if (mid) {
        parts.push_back(text("  사용자:"));
        auto user_elem = text(username);
        user_elem = (username == client::app::AppState::kDefaultUser)
                        ? user_elem | color(Color::Yellow)
                        : user_elem | bold;
        parts.push_back(user_elem);
        parts.push_back(text("  방:"));
        parts.push_back(text(room) | bold);
    } else if (small) {
        parts.push_back(text("  "));
        auto combo_elem = text(username + "@" + room);
        combo_elem = (username == client::app::AppState::kDefaultUser)
                         ? combo_elem | color(Color::Yellow)
                         : combo_elem | bold;
        parts.push_back(combo_elem);
    }

    parts.push_back(filler());
    if (wide) {
        parts.push_back(text(" F1 도움말  F5 새로고침  ESC 종료  ←/→ 패널 너비 조절  Enter 메시지 전송 ") | dim);
    } else if (mid) {
        parts.push_back(text(" F1 도움말  F5 새로고침 ") | dim);
    } else if (small) {
        parts.push_back(text(" F1 Help  F5 Refresh ") | dim);
    }

    return hbox(std::move(parts)) | bgcolor(Color::GrayDark) | color(Color::White);
}

} // namespace client::ui

