#include "chat_blacklist_state.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace server::app::chat {

namespace {

std::string trim_copy(std::string value) {
    const auto begin = value.find_first_not_of(' ');
    if (begin == std::string::npos) {
        return std::string();
    }
    const auto end = value.find_last_not_of(' ');
    return value.substr(begin, end - begin + 1);
}

} // namespace

BlacklistCommand parse_blacklist_command(std::string_view text) {
    BlacklistCommand command{};
    if (text.rfind("/block ", 0) == 0) {
        command.kind = BlacklistCommandKind::add;
        command.target_user = trim_copy(std::string(text.substr(7)));
        return command;
    }
    if (text.rfind("/unblock ", 0) == 0) {
        command.kind = BlacklistCommandKind::remove;
        command.target_user = trim_copy(std::string(text.substr(9)));
        return command;
    }
    if (text.rfind("/blacklist", 0) != 0) {
        return command;
    }

    std::string args = trim_copy(std::string(text.substr(10)));
    if (args.empty() || args == "list") {
        command.kind = BlacklistCommandKind::list;
        return command;
    }

    std::istringstream iss(args);
    std::string op;
    iss >> op;
    std::getline(iss, command.target_user);
    command.target_user = trim_copy(std::move(command.target_user));
    std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (op == "add") {
        command.kind = BlacklistCommandKind::add;
    } else if (op == "remove") {
        command.kind = BlacklistCommandKind::remove;
    }
    return command;
}

std::vector<std::string> collect_blacklisted_users_locked(const ChatServiceState& state, std::string_view actor) {
    std::vector<std::string> blocked_users;
    if (const auto it = state.user_blacklists.find(std::string(actor)); it != state.user_blacklists.end()) {
        blocked_users.assign(it->second.begin(), it->second.end());
    }
    std::sort(blocked_users.begin(), blocked_users.end());
    return blocked_users;
}

bool add_blacklist_entry_locked(ChatServiceState& state, std::string_view actor, std::string_view target_user) {
    return state.user_blacklists[std::string(actor)].insert(std::string(target_user)).second;
}

bool remove_blacklist_entry_locked(ChatServiceState& state, std::string_view actor, std::string_view target_user) {
    const auto it = state.user_blacklists.find(std::string(actor));
    if (it == state.user_blacklists.end()) {
        return false;
    }
    const bool changed = it->second.erase(std::string(target_user)) > 0;
    if (it->second.empty()) {
        state.user_blacklists.erase(it);
    }
    return changed;
}

} // namespace server::app::chat
