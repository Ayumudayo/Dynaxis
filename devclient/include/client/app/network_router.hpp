#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ftxui {
class ScreenInteractive;
}

class NetClient;

namespace client::app {

class AppState;

class NetworkRouter {
public:
    using LogSink = std::function<void(const std::string&)>;
    using RefreshRequest = std::function<void()>;

    NetworkRouter(AppState& state,
                  ::NetClient& net,
                  ftxui::ScreenInteractive& screen,
                  RefreshRequest request_refresh,
                  LogSink log_sink);

    void Initialize();

private:
    void HandleSystemRoomsBroadcast(const std::string& payload);
    void HandleSystemRoomNotice(const std::string& room, const std::string& text);

    AppState& state_;
    ::NetClient& net_;
    ftxui::ScreenInteractive& screen_;
    RefreshRequest request_refresh_;
    LogSink log_sink_;
};

} // namespace client::app

