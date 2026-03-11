#pragma once

#include <memory>

#include "server/core/storage/connection_pool.hpp"
#include "server/storage/unit_of_work.hpp"

namespace server::storage {

/** @brief 저장소 전용 unit-of-work를 생성할 수 있는 연결 풀 계약입니다. */
class IRepositoryConnectionPool : public server::core::storage::IConnectionPool {
public:
    ~IRepositoryConnectionPool() override = default;

    std::unique_ptr<server::core::storage::IUnitOfWork> make_unit_of_work() override {
        return make_repository_unit_of_work();
    }

    virtual std::unique_ptr<IRepositoryUnitOfWork> make_repository_unit_of_work() = 0;
};

} // namespace server::storage
