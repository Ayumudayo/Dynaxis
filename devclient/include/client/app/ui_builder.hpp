#pragma once

class NetClient;

#include <functional>
#include <memory>

#include <ftxui/component/component.hpp>

namespace client::app {

class AppState;
class CommandProcessor;

class UiBuilder {
public:
    using RefreshRequest = std::function<void()>;

    UiBuilder(AppState& state,
              CommandProcessor& commands,
              ::NetClient& net,
              RefreshRequest request_refresh);

    struct UiComponents {
        ftxui::Component root;
        ftxui::Component input;
    };

    UiComponents Build();

private:
    ftxui::Component BuildLeftPane();
    ftxui::Component BuildRightPane();
    ftxui::Component BuildInputBox();
    ftxui::Element BuildHelpOverlay();

    AppState& state_;
    CommandProcessor& commands_;
    ::NetClient& net_;
    RefreshRequest request_refresh_;

    ftxui::Component rooms_menu_;
    ftxui::Component users_menu_;
    ftxui::Component log_menu_;
    ftxui::Component input_box_;
};

} // namespace client::app

