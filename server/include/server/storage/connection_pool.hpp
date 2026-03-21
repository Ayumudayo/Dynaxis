#pragma once

#include <memory>

#include "server/core/storage_execution/connection_pool.hpp"
#include "server/storage/unit_of_work.hpp"

namespace server::storage {

/**
 * @brief 저장소 전용 unit-of-work를 생성할 수 있는 연결 풀 계약입니다.
 *
 * core의 `IConnectionPool`이 generic execution seam이라면, 이 인터페이스는 repository-aware
 * UoW를 추가로 제공하는 앱 계층 확장입니다. 즉 공용 transaction 경계는 재사용하되,
 * 도메인 저장소 묶음은 여전히 server 쪽에서 결정한다는 뜻입니다.
 */
class IRepositoryConnectionPool : public server::core::storage_execution::IConnectionPool {
public:
    ~IRepositoryConnectionPool() override = default;

    std::unique_ptr<server::core::storage_execution::IUnitOfWork> make_unit_of_work() override {
        return make_repository_unit_of_work();
    }

    virtual std::unique_ptr<IRepositoryUnitOfWork> make_repository_unit_of_work() = 0;
};

} // namespace server::storage
