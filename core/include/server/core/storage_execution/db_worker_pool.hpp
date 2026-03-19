#pragma once

#include "server/core/storage_execution/connection_pool.hpp"
#include "server/core/storage/db_worker_pool.hpp"

namespace server::core::storage_execution {

/** @brief generic unit-of-work seam 위에서 비동기 storage task를 실행하는 canonical worker pool입니다. */
using DbWorkerPool = server::core::storage::DbWorkerPool;

} // namespace server::core::storage_execution
