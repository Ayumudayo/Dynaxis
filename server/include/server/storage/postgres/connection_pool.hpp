#pragma once

#include <memory>
#include <string>
#include "server/core/storage/connection_pool.hpp"

namespace server::storage::postgres {

// Postgres 연결 풀 팩토리(구현은 libpqxx 기반 예정)
std::shared_ptr<server::core::storage::IConnectionPool>
make_connection_pool(const std::string& db_uri,
                     const server::core::storage::PoolOptions& opts);

} // namespace server::storage::postgres

