#pragma once

#include "server/core/storage/unit_of_work.hpp"

namespace server::core::storage_execution {

/** @brief 도메인 저장소 accessor를 포함하지 않는 generic transaction 경계입니다. */
using IUnitOfWork = server::core::storage::IUnitOfWork;

} // namespace server::core::storage_execution
