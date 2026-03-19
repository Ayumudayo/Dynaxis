#pragma once

#include "server/core/storage_execution/unit_of_work.hpp"
#include "server/storage/repositories.hpp"

namespace server::storage {

/** @brief 저장소 인터페이스 집합을 하나의 트랜잭션 경계로 묶는 계약입니다. */
class IRepositoryUnitOfWork : public server::core::storage_execution::IUnitOfWork {
public:
    ~IRepositoryUnitOfWork() override = default;

    virtual IUserRepository& users() = 0;
    virtual IRoomRepository& rooms() = 0;
    virtual IMessageRepository& messages() = 0;
    virtual ISessionRepository& sessions() = 0;
    virtual IMembershipRepository& memberships() = 0;
};

} // namespace server::storage
