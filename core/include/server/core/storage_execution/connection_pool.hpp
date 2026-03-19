#pragma once

#include "server/core/storage_execution/unit_of_work.hpp"
#include "server/core/storage/connection_pool.hpp"

namespace server::core::storage_execution {

/** @brief storage execution adapter가 공유하는 connection-pool tuning 계약입니다. */
using PoolOptions = server::core::storage::PoolOptions;

/** @brief generic unit-of-work 생성과 health check를 노출하는 storage execution adapter 경계입니다. */
using IConnectionPool = server::core::storage::IConnectionPool;

} // namespace server::core::storage_execution
