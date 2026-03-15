#include "server/app/core_internal_adapter.hpp"

#include <atomic>

#include <boost/asio/ip/tcp.hpp>

#include "server/core/net/acceptor.hpp"
#include "server/core/net/connection_runtime_state.hpp"
#include "server/core/net/session.hpp"
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/db_worker_pool.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/state/instance_registry.hpp"
#include "server/core/util/service_registry.hpp"
#include "server/core/util/crash_handler.hpp"
#include "server/storage/connection_pool.hpp"
#include "server/storage/postgres/connection_pool.hpp"
#include "server/storage/redis/factory.hpp"
#include "server/core/state/redis_backend_factory.hpp"

/**
 * @brief server_app와 server_core 내부 구현을 연결하는 부트스트랩 어댑터 구현부입니다.
 *
 * 생성/수명주기/레지스트리 헬퍼 호출을 이 단위로 집중시켜
 * 앱 부트스트랩 코드가 구체 core 구현 상세와 직접 결합되지 않도록 유지합니다.
 */
namespace server::app::core_internal {

std::shared_ptr<server::core::net::ConnectionRuntimeState> make_connection_runtime_state() {
    return std::make_shared<server::core::net::ConnectionRuntimeState>();
}

std::shared_ptr<SessionListenerHandle> make_session_listener_handle(
    boost::asio::io_context& io,
    unsigned short port,
    server::core::Dispatcher& dispatcher,
    server::core::BufferManager& buffer_manager,
    const std::shared_ptr<server::core::SessionOptions>& options,
    const std::shared_ptr<server::core::net::ConnectionRuntimeState>& state,
    SessionCloseCallback on_session_close) {
    using tcp = boost::asio::ip::tcp;

    tcp::endpoint endpoint(tcp::v4(), port);
    auto listener = std::make_shared<server::core::net::SessionListener>(
        io,
        endpoint,
        dispatcher,
        buffer_manager,
        options,
        state,
        [callback = on_session_close](std::shared_ptr<server::core::Session> session) {
            if (!callback) {
                return;
            }
            session->set_on_close(
                [callback](std::shared_ptr<server::core::Session> closing_session) {
                    callback(closing_session);
                });
        });

    auto handle = std::make_shared<SessionListenerHandle>();
    handle->start = [listener]() { listener->start(); };
    handle->stop = [listener]() { listener->stop(); };
    return handle;
}

std::shared_ptr<server::core::storage::IConnectionPool> make_postgres_connection_pool(
    const std::string& db_uri,
    std::size_t min_size,
    std::size_t max_size,
    std::uint32_t connect_timeout_ms,
    std::uint32_t query_timeout_ms,
    bool prepare_statements) {
    server::core::storage::PoolOptions options{};
    options.min_size = min_size;
    options.max_size = max_size;
    options.connect_timeout_ms = connect_timeout_ms;
    options.query_timeout_ms = query_timeout_ms;
    options.prepare_statements = prepare_statements;
    return server::storage::postgres::make_connection_pool(db_uri, options);
}

std::shared_ptr<server::storage::IRepositoryConnectionPool> make_repository_connection_pool(
    const std::string& db_uri,
    std::size_t min_size,
    std::size_t max_size,
    std::uint32_t connect_timeout_ms,
    std::uint32_t query_timeout_ms,
    bool prepare_statements) {
    server::core::storage::PoolOptions options{};
    options.min_size = min_size;
    options.max_size = max_size;
    options.connect_timeout_ms = connect_timeout_ms;
    options.query_timeout_ms = query_timeout_ms;
    options.prepare_statements = prepare_statements;
    return server::storage::postgres::make_connection_pool(db_uri, options);
}

std::shared_ptr<server::core::storage::redis::IRedisClient> make_redis_client(
    const std::string& redis_uri,
    const server::core::storage::redis::Options& options) {
    return server::storage::redis::make_redis_client(redis_uri, options);
}

std::shared_ptr<server::core::state::IInstanceStateBackend> make_registry_backend(
    const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
    const std::string& key_prefix,
    std::chrono::seconds ttl) {
    if (!redis_client) {
        return {};
    }

    return server::core::state::make_redis_registry_backend(redis_client, key_prefix, ttl);
}

bool connection_pool_health_check(
    const std::shared_ptr<server::core::storage::IConnectionPool>& connection_pool) noexcept {
    try {
        return connection_pool && connection_pool->health_check();
    } catch (...) {
        return false;
    }
}

std::shared_ptr<server::core::storage::DbWorkerPool> make_db_worker_pool(
    const std::shared_ptr<server::core::storage::IConnectionPool>& connection_pool,
    std::size_t queue_capacity) {
    return std::make_shared<server::core::storage::DbWorkerPool>(connection_pool, queue_capacity);
}

void start_db_worker_pool(
    const std::shared_ptr<server::core::storage::DbWorkerPool>& worker_pool,
    std::size_t thread_count) {
    if (!worker_pool) {
        return;
    }
    worker_pool->start(thread_count);
}

void stop_db_worker_pool(
    const std::shared_ptr<server::core::storage::DbWorkerPool>& worker_pool) noexcept {
    if (!worker_pool) {
        return;
    }
    try {
        worker_pool->stop();
    } catch (...) {
    }
}

std::uint64_t connection_count(
    const std::shared_ptr<server::core::net::ConnectionRuntimeState>& state) noexcept {
    if (!state) {
        return 0;
    }
    return state->connection_count.load(std::memory_order_relaxed);
}

void register_connection_runtime_state_service(
    const std::shared_ptr<server::core::net::ConnectionRuntimeState>& state) {
    server::core::util::services::set(state);
}

void register_connection_pool_service(
    const std::shared_ptr<server::core::storage::IConnectionPool>& connection_pool) {
    server::core::util::services::set(connection_pool);
}

void register_repository_connection_pool_service(
    const std::shared_ptr<server::storage::IRepositoryConnectionPool>& connection_pool) {
    server::core::util::services::set(connection_pool);
}

void register_db_worker_pool_service(
    const std::shared_ptr<server::core::storage::DbWorkerPool>& worker_pool) {
    server::core::util::services::set(worker_pool);
}

void install_crash_handler() {
    server::core::util::crash::install();
}

} // namespace server::app::core_internal
