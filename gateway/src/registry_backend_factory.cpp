#include "gateway/registry_backend_factory.hpp"

#include <utility>

#include "server/state/instance_registry.hpp"

namespace gateway {

std::shared_ptr<server::core::state::IInstanceStateBackend>
make_registry_backend(const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
                      std::string key_prefix,
                      std::chrono::seconds ttl) {
    if (!redis_client) {
        return {};
    }

    auto state_client = server::state::make_redis_state_client(redis_client);
    return std::make_shared<server::state::RedisInstanceStateBackend>(
        std::move(state_client),
        std::move(key_prefix),
        ttl);
}

} // namespace gateway
