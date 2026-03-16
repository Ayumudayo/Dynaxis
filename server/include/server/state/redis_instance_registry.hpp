#pragma once

#include <memory>

#include "server/core/state/redis_instance_registry.hpp"

namespace server::state {

using RedisInstanceStateBackend = server::core::state::RedisInstanceStateBackend;

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::core::storage::redis::IRedisClient> client);

} // namespace server::state
