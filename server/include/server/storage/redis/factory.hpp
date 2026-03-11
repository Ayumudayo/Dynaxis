#pragma once

#include <memory>
#include <string>

#include "server/core/storage/redis/client.hpp"

namespace server::storage::redis {

using server::core::storage::redis::Options;
using server::core::storage::redis::IRedisClient;

std::shared_ptr<IRedisClient> make_redis_client(const std::string& uri, const Options& opts);

} // namespace server::storage::redis
