#include "chat_command_authorization.hpp"

#include "chat_room_state.hpp"

namespace server::app::chat {

std::string actor_name_locked(const ChatServiceState& state, const server::core::net::Session& session) {
    if (auto actor_it = state.user.find(const_cast<server::core::net::Session*>(&session));
        actor_it != state.user.end()) {
        return actor_it->second;
    }
    return "guest";
}

bool actor_can_manage_room_locked(
    const ChatServiceState& state,
    const ChatServiceRuntimeState& runtime,
    const server::core::net::Session& session,
    std::string_view room_name) {
    const std::string actor = actor_name_locked(state, session);
    if (actor == "guest") {
        return false;
    }
    if (runtime.admin_users.count(actor) > 0) {
        return true;
    }
    const auto owner = find_room_owner_locked(state, room_name);
    return owner.has_value() && *owner == actor;
}

bool actor_is_admin_locked(
    const ChatServiceState& state,
    const ChatServiceRuntimeState& runtime,
    const server::core::net::Session& session) {
    const std::string actor = actor_name_locked(state, session);
    return actor != "guest" && runtime.admin_users.count(actor) > 0;
}

} // namespace server::app::chat
