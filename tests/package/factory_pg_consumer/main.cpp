#include <memory>
#include <string>

#include "server/core/storage/connection_pool.hpp"
#include "server/storage/connection_pool.hpp"
#include "server/storage/postgres/connection_pool.hpp"

int main() {
    server::core::storage::PoolOptions options{};
    options.min_size = 1;
    options.max_size = 2;

    auto* factory = &server::storage::postgres::make_connection_pool;
    [[maybe_unused]] std::shared_ptr<server::storage::IRepositoryConnectionPool> (*typed_factory)(
        const std::string&,
        const server::core::storage::PoolOptions&) = factory;

    return options.max_size > options.min_size ? 0 : 1;
}
