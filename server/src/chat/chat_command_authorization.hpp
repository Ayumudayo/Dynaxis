#pragma once

#include <string>
#include <string_view>

#include "chat_service_state.hpp"

namespace server::app::chat {

std::string actor_name_locked(const ChatServiceState& state, const server::core::net::Session& session);

bool actor_can_manage_room_locked(
    const ChatServiceState& state,
    const ChatServiceRuntimeState& runtime,
    const server::core::net::Session& session,
    std::string_view room_name);

bool actor_is_admin_locked(
    const ChatServiceState& state,
    const ChatServiceRuntimeState& runtime,
    const server::core::net::Session& session);

} // namespace server::app::chat
