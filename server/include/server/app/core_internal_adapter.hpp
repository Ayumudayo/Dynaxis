#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "server/core/discovery/instance_registry.hpp"
#include "server/core/storage_execution/connection_pool.hpp"
#include "server/core/storage_execution/db_worker_pool.hpp"

namespace boost::asio {
class io_context;
}

namespace server::core {
class Dispatcher;
class BufferManager;
struct SessionOptions;
class Session;
}

namespace server::core::storage::redis {
struct Options;
class IRedisClient;
}

namespace server::storage {
class IRepositoryConnectionPool;
}

namespace server::core::net {
struct ConnectionRuntimeState;
}

namespace server::app::core_internal {

/** @brief core 세션 종료 시 app-local 후처리를 연결하기 위한 콜백 타입입니다. */
using SessionCloseCallback = std::function<void(std::shared_ptr<server::core::Session>)>;

/** @brief 세션 리스너의 시작/중지 호출을 app 코드가 다루기 쉬운 형태로 묶은 핸들입니다. */
struct SessionListenerHandle {
    std::function<void()> start;
    std::function<void()> stop;
};

/**
 * @brief 세션이 공유하는 런타임 연결 카운터를 생성합니다.
 * @return 새 `ConnectionRuntimeState` 인스턴스
 */
std::shared_ptr<server::core::net::ConnectionRuntimeState> make_connection_runtime_state();

/**
 * @brief `server_core` 네트워킹 표면을 현재 `server_app` 부트스트랩에 연결하는 얇은 어댑터를 생성합니다.
 *
 * 이 계층은 영구 아키텍처 목표라기보다, 새 core 런타임 소유권 모델과 기존 app 부트스트랩이 공존하도록 하는 bridge입니다.
 * 이런 접합부가 없으면 app 코드가 다시 `server_core` 내부 구현을 직접 알아야 하고, 반대로 core가 app 개념을 품게 됩니다.
 *
 * @param io accept 루프에서 사용할 Asio IO 컨텍스트
 * @param port TCP 리스닝 포트
 * @param dispatcher accept된 세션에 적용할 패킷 디스패처
 * @param buffer_manager 세션 IO용 공유 버퍼 매니저
 * @param options accept된 세션에 적용할 세션 옵션
 * @param state 연결 카운터 공유 런타임 상태
 * @param on_session_close 각 세션에 설치할 종료 콜백
 * @return `start`/`stop` 클로저를 제공하는 리스너 핸들
 */
std::shared_ptr<SessionListenerHandle> make_session_listener_handle(
    boost::asio::io_context& io,
    unsigned short port,
    server::core::Dispatcher& dispatcher,
    server::core::BufferManager& buffer_manager,
    const std::shared_ptr<server::core::SessionOptions>& options,
    const std::shared_ptr<server::core::net::ConnectionRuntimeState>& state,
    SessionCloseCallback on_session_close);

/**
 * @brief `server_core` 저장소 옵션으로 Postgres 커넥션 풀을 생성합니다.
 *
 * app이 직접 구체 구현 타입을 new 하지 않고 이 seam을 거치게 해, 저장소 실행 계층을 교체하거나 테스트 대역을 둘 여지를 남깁니다.
 *
 * @param db_uri Postgres 연결 URI
 * @param min_size 최소 풀 크기
 * @param max_size 최대 풀 크기
 * @param connect_timeout_ms 연결 타임아웃(ms)
 * @param query_timeout_ms 쿼리 타임아웃(ms)
 * @param prepare_statements prepared statement 사용 여부
 * @return 공유 커넥션 풀 인터페이스
 */
std::shared_ptr<server::core::storage_execution::IConnectionPool> make_postgres_connection_pool(
    const std::string& db_uri,
    std::size_t min_size,
    std::size_t max_size,
    std::uint32_t connect_timeout_ms,
    std::uint32_t query_timeout_ms,
    bool prepare_statements);

std::shared_ptr<server::storage::IRepositoryConnectionPool> make_repository_connection_pool(
    const std::string& db_uri,
    std::size_t min_size,
    std::size_t max_size,
    std::uint32_t connect_timeout_ms,
    std::uint32_t query_timeout_ms,
    bool prepare_statements);

std::shared_ptr<server::core::storage::redis::IRedisClient> make_redis_client(
    const std::string& redis_uri,
    const server::core::storage::redis::Options& options);

std::shared_ptr<server::core::discovery::IInstanceStateBackend> make_registry_backend(
    const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis_client,
    const std::string& key_prefix,
    std::chrono::seconds ttl);

/**
 * @brief 커넥션 풀 헬스체크를 예외 안전하게 수행합니다.
 * @param connection_pool 점검할 커넥션 풀
 * @return 풀이 존재하고 정상 상태면 `true`
 */
bool connection_pool_health_check(
    const std::shared_ptr<server::core::storage_execution::IConnectionPool>& connection_pool) noexcept;

/**
 * @brief 커넥션 풀에 연결된 DB worker 풀을 생성합니다.
 *
 * app 부트스트랩은 "몇 스레드를 돌릴지"만 알면 되고, 실제 worker 구현 세부는 이 접합부 뒤로 숨깁니다.
 *
 * @param connection_pool worker가 사용할 커넥션 풀
 * @param queue_capacity worker 풀 최대 대기 작업 수
 * @return 공유 DB worker 풀 인스턴스
 */
std::shared_ptr<server::core::storage_execution::DbWorkerPool> make_db_worker_pool(
    const std::shared_ptr<server::core::storage_execution::IConnectionPool>& connection_pool,
    std::size_t queue_capacity);

/**
 * @brief DB worker 풀이 존재하면 worker 스레드를 시작합니다.
 * @param worker_pool 대상 DB worker 풀
 * @param thread_count 시작할 worker 스레드 수
 */
void start_db_worker_pool(
    const std::shared_ptr<server::core::storage_execution::DbWorkerPool>& worker_pool,
    std::size_t thread_count);

/**
 * @brief 예외 안전 가드와 함께 DB worker 풀을 중지합니다.
 * @param worker_pool 대상 DB worker 풀
 */
void stop_db_worker_pool(
    const std::shared_ptr<server::core::storage_execution::DbWorkerPool>& worker_pool) noexcept;

/**
 * @brief 런타임 상태에서 활성 연결 수를 읽습니다.
 * @param state 연결 카운터를 보유한 런타임 상태
 * @return 현재 활성 연결 수
 */
std::uint64_t connection_count(
    const std::shared_ptr<server::core::net::ConnectionRuntimeState>& state) noexcept;

/**
 * @brief 런타임 연결 상태를 서비스 레지스트리에 등록합니다.
 * @param state 런타임 연결 상태 서비스 인스턴스
 */
void register_connection_runtime_state_service(
    const std::shared_ptr<server::core::net::ConnectionRuntimeState>& state);

/**
 * @brief DB 커넥션 풀을 서비스 레지스트리에 등록합니다.
 * @param connection_pool 커넥션 풀 서비스 인스턴스
 */
void register_connection_pool_service(
    const std::shared_ptr<server::core::storage_execution::IConnectionPool>& connection_pool);

void register_repository_connection_pool_service(
    const std::shared_ptr<server::storage::IRepositoryConnectionPool>& connection_pool);

/**
 * @brief DB worker 풀을 서비스 레지스트리에 등록합니다.
 * @param worker_pool DB worker 풀 서비스 인스턴스
 */
void register_db_worker_pool_service(
    const std::shared_ptr<server::core::storage_execution::DbWorkerPool>& worker_pool);

/** @brief `server_core` 유틸리티의 프로세스 크래시 핸들러를 설치합니다. app이 구현 세부를 직접 알지 않게 하는 브리지 함수입니다. */
void install_crash_handler();

} // namespace server::app::core_internal
