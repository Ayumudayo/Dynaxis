#include "gateway/registry_backend_factory.hpp"

#include <utility>

#include "server/state/redis_instance_registry.hpp"
#include "server/storage/redis/factory.hpp"

namespace gateway {

std::shared_ptr<server::core::storage::redis::IRedisClient>
make_redis_client(const std::string& redis_uri,
                  const server::core::storage::redis::Options& options) {
    return server::storage::redis::make_redis_client(redis_uri, options);
}

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
