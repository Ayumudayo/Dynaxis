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

UiBuilder::UiComponents UiBuilder::Build() {
    MenuOption rooms_opt;
    rooms_opt.on_change = [this] {
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
        if (state_.show_help()) {
            return dbox({base, BuildHelpOverlay()});
        }
        return base;
    });

    return {root, input_box_};
}

Component UiBuilder::BuildLeftPane() {
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

Component UiBuilder::BuildRightPane() {
    return Renderer(log_menu_, [this] {
        auto terminal_size = Terminal::Size();
        const int width = terminal_size.dimx;
        const bool mid = width >= 80;
        const bool small = width >= 60;

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

Component UiBuilder::BuildInputBox() {
    InputOption option;
    option.placeholder = "메시지를 입력하세요...";
    option.multiline = false;
    return Input(&state_.input_buffer(), option);
}

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

