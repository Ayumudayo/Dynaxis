#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>

class NetClient;

namespace client::app {

class AppState;

class CommandProcessor {
public:
    using LogSink = std::function<void(const std::string&)>;

    CommandProcessor(AppState& state, ::NetClient& net, LogSink log_sink);

    bool Process(const std::string& line);

private:
    bool HandleCommand(const std::string& line);
    bool HandleLogin(const std::string& args);
    bool HandleJoin(const std::string& args);
    bool HandleWhisper(const std::string& args);
    bool HandleLeave(const std::string& args);
    bool HandleRefresh(const std::string& args);

    void PrintUsage(const std::string& message);
    void Warn(const std::string& message);

    using Handler = bool (CommandProcessor::*)(const std::string& args);
    struct CommandHandlerEntry {
        std::string_view command;
        Handler handler;
    };
    static const std::array<CommandHandlerEntry, 5>& command_table();

    AppState& state_;
    ::NetClient& net_;
    LogSink log_sink_;
};

} // namespace client::app

