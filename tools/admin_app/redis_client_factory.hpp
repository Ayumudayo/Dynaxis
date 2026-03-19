#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "server/core/discovery/instance_registry.hpp"
#include "server/core/storage/redis/client.hpp"

namespace admin_app {

std::shared_ptr<server::core::storage::redis::IRedisClient>
make_redis_client(const std::string& redis_uri,
                  const server::core::storage::redis::Options& options);

std::shared_ptr<server::core::discovery::IInstanceStateBackend>
make_registry_backend(const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
                      std::string key_prefix,
                      std::chrono::seconds ttl);

} // namespace admin_app
