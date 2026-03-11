#pragma once

#include <memory>

#include "server/core/storage/connection_pool.hpp"
#include "server/storage/unit_of_work.hpp"

namespace server::storage {

class IRepositoryConnectionPool : public server::core::storage::IConnectionPool {
public:
    ~IRepositoryConnectionPool() override = default;

    std::unique_ptr<server::core::storage::IUnitOfWork> make_unit_of_work() override {
        return make_repository_unit_of_work();
    }

    virtual std::unique_ptr<IRepositoryUnitOfWork> make_repository_unit_of_work() = 0;
};

} // namespace server::storage
