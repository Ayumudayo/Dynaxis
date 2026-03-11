#pragma once

#include "server/core/storage/unit_of_work.hpp"
#include "server/storage/repositories.hpp"

namespace server::storage {

class IRepositoryUnitOfWork : public server::core::storage::IUnitOfWork {
public:
    ~IRepositoryUnitOfWork() override = default;

    virtual IUserRepository& users() = 0;
    virtual IRoomRepository& rooms() = 0;
    virtual IMessageRepository& messages() = 0;
    virtual ISessionRepository& sessions() = 0;
    virtual IMembershipRepository& memberships() = 0;
};

} // namespace server::storage
