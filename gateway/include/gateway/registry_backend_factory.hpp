#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "server/core/state/instance_registry.hpp"
#include "server/core/storage/redis/client.hpp"

namespace gateway {

std::shared_ptr<server::core::state::IInstanceStateBackend>
make_registry_backend(const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
                      std::string key_prefix,
                      std::chrono::seconds ttl);

} // namespace gateway
