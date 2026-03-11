#include <memory>
#include <string>

#include "server/core/storage/redis/client.hpp"
#include "server/storage/redis/factory.hpp"

int main() {
    server::core::storage::redis::Options options{};
    options.pool_max = 4;
    options.use_streams = true;

    auto* factory = &server::storage::redis::make_redis_client;
    [[maybe_unused]] std::shared_ptr<server::core::storage::redis::IRedisClient> (*typed_factory)(
        const std::string&,
        const server::core::storage::redis::Options&) = factory;

    return options.pool_max > 0 ? 0 : 1;
}
