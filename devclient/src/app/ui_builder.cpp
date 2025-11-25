#include "client/app/ui_builder.hpp"

#include "client/app/app_state.hpp"
#include "client/app/command_processor.hpp"
#include "client/net_client.hpp"
#include "client/ui/status_bar.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <utility>

namespace client::app {

using namespace ftxui;

UiBuilder::UiBuilder(AppState& state,
                     CommandProcessor& commands,
                     ::NetClient& net,
                     RefreshRequest request_refresh)
    : state_(state),
      commands_(commands),
      net_(net),
      request_refresh_(std::move(request_refresh)) {}

// -----------------------------------------------------------------------------
// UI 레이아웃 구성
// -----------------------------------------------------------------------------
// 전체 UI 구조를 생성하고 반환합니다.
// 구조:
// [상단 상태바]
// [좌측 패널(방/유저)] | [우측 패널(채팅 로그)]
// [입력창]
UiBuilder::UiComponents UiBuilder::Build() {
    // 방 목록 메뉴 설정
    MenuOption rooms_opt;
    rooms_opt.on_change = [this] {
        // 방 선택이 바뀌면 preview를 갱신하고 서버에 /who를 보낸다.
        auto& rooms = state_.rooms();
        const int index = state_.rooms_selected();
        if (index >= 0 && index < static_cast<int>(rooms.size())) {
            const auto& preview = rooms[static_cast<std::size_t>(index)];
            state_.set_preview_room(preview);
            if (state_.connected()) {
                net_.send_who(preview);
            }
        }
        request_refresh_();
    };
    // 잠긴 방은 자물쇠 아이콘을 표시
    rooms_opt.entries_option.transform = [this](const EntryState& entry_state) {
        std::string label = entry_state.label;
        if (entry_state.index >= 0 &&
            entry_state.index < static_cast<int>(state_.rooms_locked().size()) &&
            state_.rooms_locked()[static_cast<std::size_t>(entry_state.index)]) {
            label = std::string(AppState::kLockIcon) + " " + label;
        }
        auto elem = text(label);
        if (entry_state.focused) {
            elem = elem | bold;
        }
        return elem;
    };

    rooms_menu_ = Menu(&state_.rooms(), &state_.rooms_selected(), rooms_opt);
    users_menu_ = Menu(&state_.users(), &state_.users_selected());

    // 로그 영역은 paragraph 렌더링을 사용하고 auto-scroll 여부를 추적한다.
    MenuOption log_option = MenuOption::Vertical();
    log_option.underline.enabled = false;
    log_option.entries_option.transform = [](const EntryState& entry_state) {
        auto elem = paragraph(entry_state.label);
        if (entry_state.focused) {
            elem = elem | bold;
        }
        return elem;
    };
    log_option.on_change = [this] {
        const auto& logs = state_.logs();
        const int last = logs.empty() ? 0 : static_cast<int>(logs.size()) - 1;
        // 사용자가 스크롤을 맨 아래로 내렸는지 확인하여 자동 스크롤 활성화
        state_.set_log_auto_scroll(state_.log_selected_ref() >= last);
    };
    log_menu_ = Menu(&state_.mutable_logs(), &state_.log_selected_ref(), log_option);

    input_box_ = BuildInputBox();

    auto left_renderer = BuildLeftPane();
    auto right_renderer = BuildRightPane();
    auto input_row = Container::Horizontal({input_box_});
    auto input_renderer = Renderer(input_row, [this] {
        return hbox({text("> "), input_box_->Render() | flex});
    });

    auto top_container = Container::Horizontal({left_renderer, right_renderer});
    auto main_container = Container::Vertical({top_container, input_renderer});

    // 전체 레이아웃 렌더링
    Component root = Renderer(main_container, [this, left_renderer, right_renderer, input_renderer]() {
        auto status = client::ui::RenderStatusBar(state_);
        auto content = hbox({
            left_renderer->Render(),
            separator(),
            right_renderer->Render() | flex,
        });
        auto base = vbox({
            status,
            separator(),
            content | flex,
            separator(),
            input_renderer->Render(),
        });
        // 도움말 오버레이 표시
        if (state_.show_help()) {
            return dbox({base, BuildHelpOverlay()});
        }
        return base;
    });

    return {root, input_box_};
}

// -----------------------------------------------------------------------------
// 좌측 패널 (방 목록/사용자 목록)
// -----------------------------------------------------------------------------
Component UiBuilder::BuildLeftPane() {
    // 좌측 패널: 방 목록 + 사용자 목록
    auto left_container = Container::Vertical({rooms_menu_, users_menu_});
    return Renderer(left_container, [this] {
        auto rooms_panel = window(text("방"), rooms_menu_->Render() | vscroll_indicator | yframe);
        auto users_panel = window(text("사용자"), users_menu_->Render() | vscroll_indicator | yframe);
        return vbox({
                   rooms_panel | flex,
                   separator(),
                   users_panel | flex,
               }) |
               size(WIDTH, EQUAL, state_.left_panel_width());
    });
}

// -----------------------------------------------------------------------------
// 우측 패널 (채팅 로그)
// -----------------------------------------------------------------------------
Component UiBuilder::BuildRightPane() {
    return Renderer(log_menu_, [this] {
        auto terminal_size = Terminal::Size();
        const int width = terminal_size.dimx;
        const bool mid = width >= 80;
        const bool small = width >= 60;

        // 상단 상태줄: 방 이름/인원수/사용자/연결 상태를 표시한다.
        Elements header_parts;
        header_parts.push_back(text(" 방:") | dim);
        header_parts.push_back(text(" "));
        header_parts.push_back(text(state_.current_room()) | bold);
        if (mid) {
            header_parts.push_back(text("  참여자:") | dim);
            header_parts.push_back(text(std::to_string(static_cast<int>(state_.users().size()))));
            header_parts.push_back(text("  사용자:") | dim);
            header_parts.push_back(text(state_.username()));
        } else if (small) {
            header_parts.push_back(text("  "));
            header_parts.push_back(text(std::to_string(static_cast<int>(state_.users().size())) + "명"));
        }
        header_parts.push_back(filler());
        header_parts.push_back(text(state_.connected() ? "●" : "○") |
                               color(state_.connected() ? Color::Green : Color::Red));

        auto header = hbox(std::move(header_parts));
        auto log_view = log_menu_->Render() | vscroll_indicator | yframe | flex;
        return vbox({
                   header,
                   separator(),
                   log_view,
               }) |
               flex;
    });
}

// -----------------------------------------------------------------------------
// 입력창
// -----------------------------------------------------------------------------
Component UiBuilder::BuildInputBox() {
    InputOption option;
    option.placeholder = "메시지를 입력하세요...";
    option.multiline = false;
    return Input(&state_.input_buffer(), option);
}

// -----------------------------------------------------------------------------
// 도움말 오버레이
// -----------------------------------------------------------------------------
ftxui::Element UiBuilder::BuildHelpOverlay() {
    auto lines = vbox({
        text(" 도움말") | bold,
        separator(),
        text(" F1  : 도움말 토글"),
        text(" F5  : 현재 방 새로 고침(/refresh)"),
        text(" Enter : 메시지 전송"),
        text(" ESC / Ctrl+C : 종료"),
        text(" ←/→ : 왼쪽 패널 폭 조절"),
        separator(),
        text(" /login <name>"),
        text(" /join <room> [password]"),
        text(" /whisper <user> <message>"),
        text(" /leave"),
        text(" /refresh"),
    });
    return window(text(" 단축키 안내 "), lines) | center | size(WIDTH, GREATER_THAN, 40) |
           size(HEIGHT, GREATER_THAN, 12);
}

} // namespace client::app
