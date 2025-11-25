#include "client/app/command_processor.hpp"

#include "client/app/app_state.hpp"
#include "client/net_client.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace client::app {

namespace {
// 문자열 왼쪽 공백 제거
std::string LTrim(std::string s) {
    auto it = std::find_if_not(s.begin(), s.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    s.erase(s.begin(), it);
    return s;
}

// 문자열 오른쪽 공백 제거
std::string RTrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

// 문자열 양쪽 공백 제거
std::string Trim(std::string s) {
    return RTrim(LTrim(std::move(s)));
}

// -----------------------------------------------------------------------------
// 명령어 정규화
// -----------------------------------------------------------------------------
// 사용자 입력을 트리밍하고 별칭(alias)을 정식 명령어로 치환합니다.
// 예: "/w" -> "/whisper"
std::string NormalizeCommand(std::string line) {
    static constexpr std::pair<std::string_view, std::string_view> kAliases[] = {
        {"/w", "/whisper"},
    };

    const auto command_end = line.find(' ');
    const std::size_t token_length = (command_end == std::string::npos) ? line.size() : command_end;
    const std::string_view command(line.data(), token_length);

    for (const auto& [alias, canonical] : kAliases) {
        if (command == alias) {
            line.replace(0, token_length, canonical);
            break;
        }
    }

    return line;
}

} // namespace

CommandProcessor::CommandProcessor(AppState& state, ::NetClient& net, LogSink log_sink)
    : state_(state), net_(net), log_sink_(std::move(log_sink)) {}

// -----------------------------------------------------------------------------
// 명령어 처리 진입점
// -----------------------------------------------------------------------------
// 입력된 라인이 명령어인지 확인하고 처리합니다.
// 명령어가 아니면(일반 채팅) false를 반환합니다.
bool CommandProcessor::Process(const std::string& line) {
    if (line.empty()) {
        return true;
    }
    if (line.front() != '/') {
        return false; // 일반 채팅은 false를 반환해 호출자가 직접 처리
    }
    return HandleCommand(line);
}

// -----------------------------------------------------------------------------
// 명령어 라우팅
// -----------------------------------------------------------------------------
// 정규화된 명령어를 파싱하여 핸들러 테이블에서 일치하는 함수를 찾아 실행합니다.
bool CommandProcessor::HandleCommand(const std::string& line) {
    const std::string normalized = NormalizeCommand(line);
    const auto command_end = normalized.find(' ');
    const std::string command = normalized.substr(0, command_end);
    const std::string args = (command_end == std::string::npos) ? std::string{} : normalized.substr(command_end + 1);

    for (const auto& entry : command_table()) {
        if (command == entry.command) {
            return (this->*(entry.handler))(args);
        }
    }

    PrintUsage("알 수 없는 명령입니다: " + line);
    return true;
}

// -----------------------------------------------------------------------------
// /login 명령어 핸들러
// -----------------------------------------------------------------------------
bool CommandProcessor::HandleLogin(const std::string& args) {
    auto trimmed = Trim(args);
    if (trimmed.empty()) {
        PrintUsage("usage: /login <name>");
        return true;
    }
    state_.set_username(trimmed);
    net_.send_login(state_.username(), "");
    return true;
}

// -----------------------------------------------------------------------------
// /join 명령어 핸들러
// -----------------------------------------------------------------------------
bool CommandProcessor::HandleJoin(const std::string& args) {
    std::string room_arg;
    std::string password_arg;

    auto rest = Trim(args);
    auto pos = rest.find(' ');
    if (pos == std::string::npos) {
        room_arg = std::move(rest);
    } else {
        room_arg = Trim(rest.substr(0, pos));
        password_arg = Trim(rest.substr(pos + 1));
    }

    if (room_arg.empty()) {
        PrintUsage("usage: /join <room> [password]");
        return true;
    }

    state_.set_pending_join_room(room_arg);
    net_.send_join(room_arg, password_arg);
    return true;
}

// -----------------------------------------------------------------------------
// /whisper 명령어 핸들러
// -----------------------------------------------------------------------------
bool CommandProcessor::HandleWhisper(const std::string& args) {
    auto trimmed = Trim(args);
    auto pos = trimmed.find(' ');
    if (pos == std::string::npos) {
        PrintUsage("usage: /whisper <user> <message>");
        return true;
    }

    auto target = Trim(trimmed.substr(0, pos));
    auto message = Trim(trimmed.substr(pos + 1));

    if (target.empty() || message.empty()) {
        PrintUsage("usage: /whisper <user> <message>");
        return true;
    }

    if (state_.username() == AppState::kDefaultUser) {
        Warn("[warn] 로그인 상태에서만 귓속말을 보낼 수 있습니다.");
        return true;
    }
    if (target == state_.username()) {
        Warn("[warn] 자기 자신에게는 귓속말을 보낼 수 없습니다.");
        return true;
    }

    net_.send_whisper(target, message);
    return true;
}

// -----------------------------------------------------------------------------
// /leave 명령어 핸들러
// -----------------------------------------------------------------------------
bool CommandProcessor::HandleLeave(const std::string& args) {
    auto trimmed = Trim(args);
    std::string room = trimmed.empty() ? state_.current_room() : std::move(trimmed);
    net_.send_leave(room);
    return true;
}

// -----------------------------------------------------------------------------
// /refresh 명령어 핸들러
// -----------------------------------------------------------------------------
// /refresh는 인자가 없으며 연결 상태에서만 허용된다.
bool CommandProcessor::HandleRefresh(const std::string& args) {
    if (!Trim(args).empty()) {
        PrintUsage("usage: /refresh");
        return true;
    }
    if (!state_.connected()) {
        Warn("연결되어 있지 않습니다.");
        return true;
    }
    net_.send_refresh(state_.current_room());
    return true;
}

// -----------------------------------------------------------------------------
// 명령어 핸들러 테이블
// -----------------------------------------------------------------------------
const std::array<CommandProcessor::CommandHandlerEntry, 5>& CommandProcessor::command_table() {
    static constexpr std::array<CommandHandlerEntry, 5> kCommandHandlers = {{
        {"/login", &CommandProcessor::HandleLogin},
        {"/join", &CommandProcessor::HandleJoin},
        {"/whisper", &CommandProcessor::HandleWhisper},
        {"/leave", &CommandProcessor::HandleLeave},
        {"/refresh", &CommandProcessor::HandleRefresh},
    }};
    return kCommandHandlers;
}

void CommandProcessor::PrintUsage(const std::string& message) {
    if (log_sink_) {
        log_sink_(message);
    }
}

void CommandProcessor::Warn(const std::string& message) {
    if (log_sink_) {
        log_sink_(message);
    }
}

} // namespace client::app
