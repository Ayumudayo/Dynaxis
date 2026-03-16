#include "server/state/redis_instance_registry.hpp"

namespace server::state {

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::core::storage::redis::IRedisClient> client) {
    return server::core::state::make_redis_state_client(std::move(client));
}

} // namespace server::state
