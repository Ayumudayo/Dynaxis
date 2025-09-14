#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <span>
#include <csignal>
#include <cstdlib> // getenv, strtoul
#include <cstring> // strcmp

#include <boost/asio.hpp>
#include <clocale>
#if defined(_WIN32)
#  include <windows.h>
#endif

#include "server/app/bootstrap.hpp"
#include "server/app/router.hpp"

#include "server/core/net/acceptor.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/net/session.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/config/options.hpp"
#include "server/core/state/shared_state.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/chat/chat_service.hpp"
// 저장소 DI: Postgres 커넥션 풀 팩토리
#include "server/storage/postgres/connection_pool.hpp"
#include "server/core/storage/connection_pool.hpp"
// 캐시/팬아웃: Redis 클라이언트(스켈레톤)
#include "server/storage/redis/client.hpp"
// .env 로더
#include "server/core/config/dotenv.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace core = server::core;
namespace protocol = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app {

int run_server(int argc, char** argv) {
    try {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        // .env가 있으면 이를 우선(override=true), 없으면 OS 환경변수 사용
        if (server::core::config::load_dotenv(".env", true)) {
            corelog::info("Loaded .env (override existing env = true)");
        } else {
            corelog::info(".env not found — using OS environment variables");
        }

        unsigned short port = 5000;
        if (argc >= 2) {
            port = static_cast<unsigned short>(std::stoi(argv[1]));
        }

        asio::io_context io;
        core::JobQueue job_queue;
        core::ThreadManager workers(job_queue);
        core::BufferManager buffer_manager(2048, 1024); // 2KB buffers, 1024 count

        core::Dispatcher dispatcher;
        auto options = std::make_shared<core::SessionOptions>();
        options->read_timeout_ms = 60'000;
        options->heartbeat_interval_ms = 10'000;
        auto state = std::make_shared<core::SharedState>();

        // DB 커넥션 풀 구성(환경 변수 기반)
        std::shared_ptr<core::storage::IConnectionPool> db_pool;
        {
            const char* uri = std::getenv("DB_URI");
            if (uri && *uri) {
                corelog::info(std::string("DB_URI 감지: ") + uri);
                core::storage::PoolOptions popts{};
                if (const char* v = std::getenv("DB_POOL_MIN")) popts.min_size = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_POOL_MAX")) popts.max_size = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_CONN_TIMEOUT_MS")) popts.connect_timeout_ms = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_QUERY_TIMEOUT_MS")) popts.query_timeout_ms = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_PREPARE_STATEMENTS")) popts.prepare_statements = (std::strcmp(v, "0") != 0);

                db_pool = server::storage::postgres::make_connection_pool(uri, popts);
                if (!db_pool || !db_pool->health_check()) {
                    corelog::error("DB 헬스체크 실패 — DB_URI를 확인하세요.");
                    return 2;
                }
                corelog::info("DB 커넥션 풀 초기화 완료.");
            } else {
                corelog::warn("DB_URI 미설정 — DB 연동 비활성(후속 단계에서 필요)");
            }
        }

        // Redis 클라이언트 구성(환경 변수 기반)
        std::shared_ptr<server::storage::redis::IRedisClient> redis;
        if (const char* ruri = std::getenv("REDIS_URI"); ruri && *ruri) {
            corelog::info(std::string("REDIS_URI 감지: ") + ruri);
            server::storage::redis::Options ropts{};
            if (const char* v = std::getenv("REDIS_POOL_MAX")) ropts.pool_max = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
            if (const char* v = std::getenv("REDIS_USE_STREAMS")) ropts.use_streams = (std::strcmp(v, "0") != 0);
            redis = server::storage::redis::make_redis_client(ruri, ropts);
            // 최소 복원: PRESENCE_CLEAN_ON_START != 0 이면 presence:room:* 정리(개발/단일 게이트웨이 전제)
            if (redis) {
                if (const char* clean = std::getenv("PRESENCE_CLEAN_ON_START"); clean && std::strcmp(clean, "0") != 0) {
                    std::string prefix;
                    if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX"); p && *p) prefix = p;
                    std::string pattern = prefix + std::string("presence:room:*");
                    corelog::warn(std::string("Presence cleanup on start: ") + pattern);
                    (void)redis->scan_del(pattern);
                }
            }
            if (!redis || !redis->health_check()) {
                corelog::error("Redis 헬스체크 실패 — REDIS_URI를 확인하세요.");
            } else {
                corelog::info("Redis 클라이언트 초기화 완료.");
            }
        } else {
            corelog::warn("REDIS_URI 미설정 — Redis 연동 비활성(후속 단계에서 필요)");
        }

        server::app::chat::ChatService chat(io, job_queue, db_pool, redis);
        // TODO: ChatService에 저장소 주입(후속 단계)

        register_routes(dispatcher, chat);

        tcp::endpoint ep(tcp::v4(), port);
        auto acceptor = std::make_shared<core::Acceptor>(io, ep, dispatcher, buffer_manager, options, state,
            [&chat](std::shared_ptr<core::Session> sess){
                sess->set_on_close([&chat](std::shared_ptr<core::Session> s){ chat.on_session_close(s); });
            });
        acceptor->start();
        corelog::info("server_app 시작: 0.0.0.0:" + std::to_string(port));

        // 워커 스레드 풀 시작
        unsigned int num_worker_threads = std::max(1u, std::thread::hardware_concurrency());
        workers.Start(num_worker_threads);
        corelog::info(std::to_string(num_worker_threads) + "개의 워커 스레드를 시작합니다.");

        // I/O 스레드 풀 시작
        unsigned int num_io_threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> io_threads;
        io_threads.reserve(num_io_threads);
        for (unsigned int i = 0; i < num_io_threads; ++i) {
            io_threads.emplace_back([&io]() { 
                try {
                    io.run();
                } catch (const std::exception& e) {
                    corelog::error(std::string("I/O 스레드 예외: ") + e.what());
                }
            });
        }
        corelog::info(std::to_string(num_io_threads) + "개의 I/O 스레드를 시작합니다.");

        // 정상 종료(Ctrl+C) 처리
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code&, int) {
            corelog::info("서버 종료 신호 수신...");
            acceptor->stop();
            io.stop();
            workers.Stop();
        });

        for (auto& t : io_threads) {
            t.join();
        }

        return 0;
    } catch (const std::exception& ex) {
        corelog::error(std::string("server_app 예외: ") + ex.what());
        return 1;
    }
}

} // namespace server::app
