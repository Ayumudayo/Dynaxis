#include "server/core/api/version.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/app/engine_builder.hpp"
#include "server/core/app/engine_context.hpp"
#include "server/core/app/engine_runtime.hpp"
#include "server/core/app/termination_signals.hpp"
#include "server/core/build_info.hpp"
#include "server/core/compression/compressor.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/config/options.hpp"
#include "server/core/discovery/instance_registry.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/http_server.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/net/connection.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"
#include "server/core/net/transport_router.hpp"
#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/security/cipher.hpp"
#include "server/core/storage_execution/connection_pool.hpp"
#include "server/core/storage_execution/db_worker_pool.hpp"
#include "server/core/storage_execution/retry_backoff.hpp"
#include "server/core/storage_execution/unit_of_work.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/paths.hpp"
#include "server/core/util/service_registry.hpp"
#include <atomic>
#include <stdexcept>
int main() {
    using namespace server::core;
    if (api::version_string() != "4.0.0") { return 1; }
    auto runtime = app::EngineBuilder("public-consumer").build();
    auto other = app::EngineBuilder("peer-consumer").build();
    runtime.set_service(std::make_shared<int>(42));
    if (other.context().get<int>()) { return 2; }
    JobQueue queue(4);
    ThreadManager workers(queue);
    std::atomic<int> completed{0};
    if (!queue.TryPush([] { throw std::runtime_error("consumer job failure"); })) { return 3; }
    if (!queue.TryPush([&] { ++completed; })) { return 4; }
    workers.Start(1);
    workers.Stop();
    if (completed != 1 || queue.TryPush([] {})) { return 5; }
    BufferManager::PooledBuffer outstanding;
    {
        BufferManager buffers(128, 2);
        outstanding = buffers.Acquire();
        if (!outstanding) { return 6; }
    }
    outstanding.reset();
    return 0;
}
