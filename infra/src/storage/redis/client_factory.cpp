#include "server/storage/redis/factory.hpp"

#include "client_impl.hpp"

namespace server::storage::redis {

std::shared_ptr<IRedisClient> make_redis_client(const std::string& uri, const Options& opts) {
    return make_redis_client_impl(uri, opts);
}

} // namespace server::storage::redis
