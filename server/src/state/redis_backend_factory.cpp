#include "server/state/redis_backend_factory.hpp"

#include <utility>

#include "server/state/redis_instance_registry.hpp"

namespace server::state {

std::shared_ptr<server::core::state::IInstanceStateBackend>
make_redis_registry_backend(const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
                            std::string key_prefix,
                            std::chrono::seconds ttl) {
    if (!redis_client) {
        return {};
    }

    auto adapter = make_redis_state_client(redis_client);
    return std::make_shared<RedisInstanceStateBackend>(
        std::move(adapter),
        std::move(key_prefix),
        ttl);
}

} // namespace server::state
