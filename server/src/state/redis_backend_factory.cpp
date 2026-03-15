#include "server/state/redis_backend_factory.hpp"

#include <utility>

namespace server::state {

std::shared_ptr<server::core::state::IInstanceStateBackend>
make_redis_registry_backend(const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
                            std::string key_prefix,
                            std::chrono::seconds ttl) {
    return server::core::state::make_redis_registry_backend(redis_client, std::move(key_prefix), ttl);
}

} // namespace server::state
