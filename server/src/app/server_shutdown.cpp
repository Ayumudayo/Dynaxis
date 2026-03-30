#include "server_shutdown.hpp"

#include "server_runtime_state.hpp"

#include "server/app/core_internal_adapter.hpp"
#include "server/core/app/engine_runtime.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/discovery/instance_registry.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/util/log.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace asio = boost::asio;

namespace server::app {

namespace corelog = server::core::log;

namespace {

void run_shutdown_drain(
    const std::shared_ptr<server::core::net::ConnectionRuntimeState>& connection_state,
    std::uint64_t timeout_ms,
    std::uint64_t poll_ms) {
    set_bootstrap_shutdown_drain_timeout(timeout_ms);
    const auto started_at = std::chrono::steady_clock::now();

    for (;;) {
        const std::uint64_t remaining = core_internal::connection_count(connection_state);
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at).count();
        update_bootstrap_shutdown_drain_progress(remaining, elapsed);

        if (remaining == 0) {
            record_bootstrap_shutdown_drain_completed();
            corelog::info("Shutdown drain completed within timeout");
            return;
        }

        if (elapsed >= static_cast<long long>(timeout_ms)) {
            record_bootstrap_shutdown_drain_timeout(remaining);
            corelog::warn(
                "Shutdown drain timeout reached; forcing close of remaining connections="
                + std::to_string(remaining));
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
}

} // namespace

void register_server_shutdown_steps(server::core::app::EngineRuntime& engine,
                                    ServerShutdownContext context) {
    engine.add_shutdown_step("stop workers", [workers = context.workers]() {
        if (workers) {
            workers->Stop();
        }
    });

    engine.add_shutdown_step("stop io_context", [io = context.io]() {
        if (io) {
            io->stop();
        }
    });

    engine.add_shutdown_step(
        "drain active sessions",
        [connection_state = context.connection_state,
         timeout_ms = context.shutdown_drain_timeout_ms,
         poll_ms = std::max<std::uint64_t>(1, context.shutdown_drain_poll_ms)]() {
            run_shutdown_drain(connection_state, timeout_ms, poll_ms);
        });

    engine.add_shutdown_step("stop acceptor", [acceptor = context.acceptor]() {
        if (acceptor) {
            acceptor->stop();
        }
    });

    engine.add_shutdown_step("stop db worker pool", [db_workers = context.db_workers]() {
        core_internal::stop_db_worker_pool(db_workers);
    });

    engine.add_shutdown_step("reset lua runtime", [lua_runtime = context.lua_runtime]() {
        if (lua_runtime) {
            lua_runtime->reset();
        }
    });

    engine.add_shutdown_step("stop redis pubsub", [redis = context.redis]() {
        try {
            if (redis) {
                redis->stop_psubscribe();
            }
        } catch (...) {
            server::core::runtime_metrics::record_exception_ignored();
        }
    });

    engine.add_shutdown_step(
        "deregister instance",
        [registry_registered = context.registry_registered,
         registry_backend = context.registry_backend,
         registry_record = context.registry_record]() {
            if (!registry_registered || !registry_record) {
                return;
            }
            if (*registry_registered && registry_backend) {
                try {
                    registry_backend->remove(registry_record->instance_id);
                } catch (...) {
                    server::core::runtime_metrics::record_exception_ignored();
                }
                *registry_registered = false;
            }
        });
}

} // namespace server::app
