#pragma once

#include <memory>
#include <string>

#include "server/core/storage/redis/client.hpp"

namespace wb_worker {

std::shared_ptr<server::core::storage::redis::IRedisClient>
make_redis_client(const std::string& redis_uri,
                  const server::core::storage::redis::Options& options);

} // namespace wb_worker
