#pragma once

#include <memory>
#include <string>

#include "server/core/storage_execution/connection_pool.hpp"
#include "server/storage/connection_pool.hpp"

namespace server::storage::postgres {

std::shared_ptr<server::storage::IRepositoryConnectionPool>
make_connection_pool_impl(const std::string& db_uri,
                          const server::core::storage_execution::PoolOptions& opts);

} // namespace server::storage::postgres
