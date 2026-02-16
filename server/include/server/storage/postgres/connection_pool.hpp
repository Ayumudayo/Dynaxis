#pragma once

#include <memory>
#include <string>
#include "server/core/storage/connection_pool.hpp"

namespace server::storage::postgres {

/**
 * @brief Postgres 연결 풀을 생성합니다.
 * @param db_uri Postgres 접속 URI
 * @param opts 풀 동작 옵션
 * @return 생성된 IConnectionPool 구현체
 */
std::shared_ptr<server::core::storage::IConnectionPool>
make_connection_pool(const std::string& db_uri,
                     const server::core::storage::PoolOptions& opts);

} // namespace server::storage::postgres
