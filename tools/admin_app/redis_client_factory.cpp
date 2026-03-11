#include "redis_client_factory.hpp"

#include "server/storage/redis/client.hpp"

namespace admin_app {

std::shared_ptr<server::core::storage::redis::IRedisClient>
make_redis_client(const std::string& redis_uri,
                  const server::core::storage::redis::Options& options) {
    return server::storage::redis::make_redis_client(redis_uri, options);
}

} // namespace admin_app
