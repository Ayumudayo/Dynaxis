#pragma once

#include <string>
#include <vector>

#include "chat_service_state.hpp"

namespace server::app::chat {

enum class BlacklistCommandKind {
    invalid,
    list,
    add,
    remove,
};

struct BlacklistCommand {
    BlacklistCommandKind kind{BlacklistCommandKind::invalid};
    std::string target_user;
};

BlacklistCommand parse_blacklist_command(std::string_view text);

std::vector<std::string> collect_blacklisted_users_locked(const ChatServiceState& state, std::string_view actor);

bool add_blacklist_entry_locked(ChatServiceState& state, std::string_view actor, std::string_view target_user);

bool remove_blacklist_entry_locked(ChatServiceState& state, std::string_view actor, std::string_view target_user);

} // namespace server::app::chat
