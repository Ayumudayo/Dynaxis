#pragma once

#include <memory>
#include <string>

#include "server/core/storage_execution/connection_pool.hpp"

namespace server::storage { class IRepositoryConnectionPool; }

namespace server::storage::postgres {

/**
 * @brief Postgres 연결 풀 구현체를 생성합니다.
 * @param db_uri Postgres 접속 URI
 * @param opts 풀 동작 옵션
 * @return 생성된 repository connection pool 구현체
 *
 * 이 factory를 별도 헤더로 두는 이유는, 저장소 계약(`IRepositoryConnectionPool`)과
 * Postgres concrete 구현 선택을 분리하기 위해서입니다. 앱 코드는 계약에 기대고,
 * 실제 DB 종류와 연결 전략은 구현 factory에서 늦게 결정하는 편이 유지보수에 유리합니다.
 */
std::shared_ptr<server::storage::IRepositoryConnectionPool>
make_connection_pool(const std::string& db_uri,
                     const server::core::storage_execution::PoolOptions& opts);

} // namespace server::storage::postgres
