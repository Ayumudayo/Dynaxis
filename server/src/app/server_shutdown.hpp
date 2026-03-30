#pragma once

#include "server/app/core_internal_adapter.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/state/instance_registry.hpp"

#include <cstdint>
#include <memory>

namespace boost::asio {
class io_context;
}

namespace server::core::app {
class EngineRuntime;
}

namespace server::core::net {
struct ConnectionRuntimeState;
}

namespace server::core::scripting {
class LuaRuntime;
}

namespace server::core::storage::redis {
class IRedisClient;
}

namespace server::app {

struct ServerShutdownContext {
    server::core::ThreadManager* workers{nullptr};
    boost::asio::io_context* io{nullptr};
    std::shared_ptr<server::core::net::ConnectionRuntimeState> connection_state;
    std::shared_ptr<server::app::core_internal::SessionListenerHandle> acceptor;
    std::shared_ptr<server::core::storage_execution::DbWorkerPool> db_workers;
    std::shared_ptr<server::core::scripting::LuaRuntime> lua_runtime;
    std::shared_ptr<server::core::storage::redis::IRedisClient> redis;
    bool* registry_registered{nullptr};
    std::shared_ptr<server::core::state::IInstanceStateBackend> registry_backend;
    server::core::state::InstanceRecord* registry_record{nullptr};
    std::uint64_t shutdown_drain_timeout_ms{0};
    std::uint64_t shutdown_drain_poll_ms{0};
};

void register_server_shutdown_steps(server::core::app::EngineRuntime& engine,
                                    ServerShutdownContext context);

} // namespace server::app
