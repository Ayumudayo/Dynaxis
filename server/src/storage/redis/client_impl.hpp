#pragma once

#include <memory>
#include <string>

#include "server/core/storage/redis/client.hpp"

namespace server::storage::redis {

std::shared_ptr<server::core::storage::redis::IRedisClient>
make_redis_client_impl(const std::string& uri,
                       const server::core::storage::redis::Options& opts);

} // namespace server::storage::redis
