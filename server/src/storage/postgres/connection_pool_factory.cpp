#include "server/storage/postgres/connection_pool.hpp"

#include "connection_pool_impl.hpp"

namespace server::storage::postgres {

std::shared_ptr<server::storage::IRepositoryConnectionPool>
make_connection_pool(const std::string& db_uri,
                     const server::core::storage::PoolOptions& opts) {
    return make_connection_pool_impl(db_uri, opts);
}

} // namespace server::storage::postgres
