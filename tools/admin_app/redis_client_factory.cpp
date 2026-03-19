#include "redis_client_factory.hpp"

#include <utility>

#include "server/core/state/redis_backend_factory.hpp"
#include "server/storage/redis/factory.hpp"

namespace admin_app {

std::shared_ptr<server::core::storage::redis::IRedisClient>
make_redis_client(const std::string& redis_uri,
                  const server::core::storage::redis::Options& options) {
    return server::storage::redis::make_redis_client(redis_uri, options);
}

std::shared_ptr<server::core::discovery::IInstanceStateBackend>
make_registry_backend(const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
                      std::string key_prefix,
                      std::chrono::seconds ttl) {
    if (!redis_client) {
        return {};
    }

    return server::core::state::make_redis_registry_backend(redis_client, std::move(key_prefix), ttl);
}

} // namespace admin_app
