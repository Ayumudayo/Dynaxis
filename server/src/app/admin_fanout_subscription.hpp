#pragma once

#include "server/app/config.hpp"
#include "server/core/state/instance_registry.hpp"

#include <memory>

namespace server::app::chat {
class ChatService;
}

namespace server::core::security::admin_command_auth {
class Verifier;
}

namespace server::core::storage::redis {
class IRedisClient;
}

namespace server::app {

std::shared_ptr<server::core::security::admin_command_auth::Verifier>
make_admin_command_verifier(const ServerConfig& config);

server::core::state::InstanceRecord
make_local_instance_selector_context(const ServerConfig& config);

void start_chat_fanout_subscription(
    chat::ChatService& chat,
    const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis,
    const ServerConfig& config,
    const std::shared_ptr<server::core::security::admin_command_auth::Verifier>& admin_command_verifier,
    const server::core::state::InstanceRecord& local_instance_selector_context);

} // namespace server::app
