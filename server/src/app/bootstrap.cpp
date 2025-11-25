#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <span>
#include <csignal>
#include <cstdlib> // getenv, strtoul
#include <cstring> // strcmp
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <type_traits>
#include <functional>

#include <boost/asio.hpp>
#include <boost/asio/streambuf.hpp>
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
#include "server/core/util/service_registry.hpp"
#include "server/core/util/paths.hpp"
#include "server/core/util/crash_handler.hpp"
#include "server/core/config/options.hpp"
#include "server/core/state/shared_state.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/chat/chat_service.hpp"
// Protobuf (수신 payload ts_ms 파싱용)
#include "wire.pb.h"
// 저장소 DI: Postgres 커넥션 풀 팩토리
#include "server/storage/postgres/connection_pool.hpp"
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/db_worker_pool.hpp"
// 캐시/팬아웃: Redis 클라이언트(스켈레톤)
#include "server/storage/redis/client.hpp"
#include "server/state/instance_registry.hpp"
// .env 로더
#include "server/core/config/dotenv.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace core = server::core;
namespace protocol = server::core::protocol;
namespace corelog = server::core::log;
namespace services = server::core::util::services;
namespace pathutil = server::core::util::paths;
namespace crash = server::core::util::crash;

namespace server::app {

// 간단 메트릭 카운터/게이지 (HTTP /metrics 엔드포인트를 통해 노출됨)
static std::atomic<std::uint64_t> g_subscribe_total{0};
static std::atomic<std::uint64_t> g_self_echo_drop_total{0};
static std::atomic<long long>     g_subscribe_last_lag_ms{0};

int run_server(int argc, char** argv) {
    // 1. 핵심 컴포넌트 선언
    // TaskScheduler: 주기적인 작업이나 지연된 작업을 실행합니다.
    core::concurrent::TaskScheduler scheduler;
    std::shared_ptr<asio::steady_timer> scheduler_timer;
    
    // DB 워커 풀: 데이터베이스 작업을 비동기적으로 처리합니다.
    std::shared_ptr<core::storage::DbWorkerPool> db_workers;
    
    // 인스턴스 레지스트리: 서버의 존재를 알리고 상태를 보고합니다.
    std::shared_ptr<server::state::RedisInstanceStateBackend> registry_backend;
    server::state::InstanceRecord registry_record{};
    std::chrono::seconds registry_heartbeat_interval{std::chrono::seconds{5}};
    bool registry_registered = false;

    try {
#if defined(_WIN32)
        // 윈도우 콘솔의 인코딩을 UTF-8로 설정하여 한글 출력이 깨지지 않도록 합니다.
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        // 크래시 핸들러 설치 (세그멘테이션 폴트 등 발생 시 스택 트레이스 출력)
        crash::install();

        // 2. 환경 변수 로드 (.env 파일)
        bool env_loaded = false;
        std::filesystem::path env_path;
        std::filesystem::path exe_dir;
        try {
            // 실행 파일이 있는 디렉토리를 기준으로 .env 파일을 찾습니다.
            exe_dir = pathutil::executable_dir();
            auto local_env = exe_dir / ".env";
            if (std::filesystem::exists(local_env)) {
                env_loaded = server::core::config::load_dotenv(local_env.string(), true);
                if (env_loaded) env_path = local_env;
            } else {
                // .env가 없으면 .env.default를 복사하여 생성합니다.
                auto local_default = exe_dir / ".env.default";
                if (std::filesystem::exists(local_default)) {
                    try {
                        std::filesystem::copy_file(local_default, local_env, std::filesystem::copy_options::overwrite_existing);
                        corelog::info("Seeded .env next to executable from .env.default");
                        env_loaded = server::core::config::load_dotenv(local_env.string(), true);
                        if (env_loaded) env_path = local_env;
                    } catch (const std::exception& copy_ex) {
                        corelog::warn(std::string("Failed to seed .env next to executable: ") + copy_ex.what());
                    }
                }
            }
        } catch (const std::exception& ex) {
            corelog::warn(std::string("Executable dir detection failed: ") + ex.what());
        }
        
        // 실행 파일 경로에서 실패했다면 현재 작업 디렉토리에서 다시 시도합니다.
        if (!env_loaded) {
            std::filesystem::path repo_env{".env"};
            if (!std::filesystem::exists(repo_env)) {
                std::filesystem::path repo_default{".env.default"};
                if (std::filesystem::exists(repo_default)) {
                    try {
                        std::filesystem::copy_file(repo_default, repo_env, std::filesystem::copy_options::overwrite_existing);
                        corelog::info("Seeded repository .env from .env.default");
                    } catch (const std::exception& copy_ex) {
                        corelog::warn(std::string("Failed to seed repository .env: ") + copy_ex.what());
                    }
                }
            }
            if (std::filesystem::exists(repo_env)) {
                env_loaded = server::core::config::load_dotenv(repo_env.string(), true);
                if (env_loaded) env_path = std::filesystem::absolute(repo_env);
            }
        }
        
        if (env_loaded) {
            corelog::info(std::string("Loaded environment file: ") + env_path.string());
        } else {
            corelog::info("No .env file located; using existing environment variables");
        }

        // 로그 버퍼 용량 설정 (환경 변수 LOG_BUFFER_CAPACITY)
        if (const char* buf_cap = std::getenv("LOG_BUFFER_CAPACITY"); buf_cap && *buf_cap) {
            auto cap = std::strtoull(buf_cap, nullptr, 10);
            if (cap > 0) {
                corelog::set_buffer_capacity(static_cast<std::size_t>(cap));
                corelog::info(std::string("Log buffer capacity set to ") + std::to_string(cap));
            }
        }

        unsigned short port = 5000;
        if (argc >= 2) {
            port = static_cast<unsigned short>(std::stoi(argv[1]));
        }

        // 3. 인스턴스 레지스트리 설정
        // 로드 밸런서(Gateway)가 이 서버에 접근할 수 있도록 호스트와 포트를 설정합니다.
        std::string advertise_host = "127.0.0.1";
        if (const char* host_env = std::getenv("SERVER_ADVERTISE_HOST"); host_env && *host_env) {
            advertise_host = host_env;
        }
        unsigned short advertise_port = port;
        if (const char* port_env = std::getenv("SERVER_ADVERTISE_PORT"); port_env && *port_env) {
            try {
                advertise_port = static_cast<unsigned short>(std::stoul(port_env));
            } catch (const std::exception& ex) {
                corelog::warn(std::string("Invalid SERVER_ADVERTISE_PORT: ") + ex.what());
            } catch (...) {
                corelog::warn("Invalid SERVER_ADVERTISE_PORT value; using listen port");
            }
        }

        if (const char* hb_env = std::getenv("SERVER_HEARTBEAT_INTERVAL"); hb_env && *hb_env) {
            try {
                auto parsed = std::stoul(hb_env);
                if (parsed > 0) {
                    registry_heartbeat_interval = std::chrono::seconds{static_cast<long long>(parsed)};
                }
            } catch (const std::exception& ex) {
                corelog::warn(std::string("Invalid SERVER_HEARTBEAT_INTERVAL: ") + ex.what());
            }
        }

        std::string registry_prefix = "gateway/instances";
        if (const char* prefix_env = std::getenv("SERVER_REGISTRY_PREFIX"); prefix_env && *prefix_env) {
            registry_prefix = prefix_env;
        }
        if (!registry_prefix.empty() && registry_prefix.back() != '/') {
            registry_prefix.push_back('/');
        }

        std::chrono::seconds registry_ttl{std::chrono::seconds{30}};
        if (const char* ttl_env = std::getenv("SERVER_REGISTRY_TTL"); ttl_env && *ttl_env) {
            try {
                auto parsed = std::stoul(ttl_env);
                if (parsed > 0) {
                    registry_ttl = std::chrono::seconds{static_cast<long long>(parsed)};
                }
            } catch (const std::exception& ex) {
                corelog::warn(std::string("Invalid SERVER_REGISTRY_TTL: ") + ex.what());
            }
        }

        // 서버 인스턴스 ID 생성 (고유 식별자)
        std::string server_instance_id;
        if (const char* id_env = std::getenv("SERVER_INSTANCE_ID"); id_env && *id_env) {
            server_instance_id = id_env;
        } else {
            server_instance_id = "server-" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        // 4. 코어 컴포넌트 초기화
        asio::io_context io;
        core::JobQueue job_queue;
        auto* job_queue_ptr = &job_queue;
        core::ThreadManager workers(job_queue);
        core::BufferManager buffer_manager(2048, 1024); // 2KB 버퍼, 1024개 미리 할당

        core::Dispatcher dispatcher;
        auto options = std::make_shared<core::SessionOptions>();
        options->read_timeout_ms = 60'000;
        options->heartbeat_interval_ms = 10'000;
        auto state = std::make_shared<core::SharedState>();

        // ServiceRegistry에 컴포넌트 등록 (의존성 주입을 위해 사용)
        const auto make_ref = [](auto& instance) {
            using T = std::remove_reference_t<decltype(instance)>;
            return std::shared_ptr<T>(&instance, [](T*) {});
        };
        services::set(make_ref(io));
        services::set(make_ref(job_queue));
        services::set(make_ref(workers));
        services::set(make_ref(buffer_manager));
        services::set(make_ref(dispatcher));
        services::set(options);
        services::set(state);

        services::set(make_ref(scheduler));

        // 스케줄러 타이머 설정 (주기적으로 스케줄러를 폴링)
        scheduler_timer = std::make_shared<asio::steady_timer>(io);
        services::set(scheduler_timer);

        auto scheduler_tick = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> scheduler_tick_weak = scheduler_tick;
        *scheduler_tick = [scheduler_timer, scheduler_tick_weak, scheduler_ptr = &scheduler]() {
            scheduler_ptr->poll(32);
            scheduler_timer->expires_after(std::chrono::milliseconds(50));
            scheduler_timer->async_wait([scheduler_timer, scheduler_tick_weak, scheduler_ptr](const boost::system::error_code& ec) {
                if (ec == asio::error::operation_aborted) {
                    return;
                }
                if (!ec) {
                    scheduler_ptr->poll(32);
                    if (auto locked = scheduler_tick_weak.lock()) {
                        (*locked)();
                    }
                }
            });
        };
        (*scheduler_tick)();

        // 5. DB 커넥션 풀 구성 (환경 변수 기반)
        std::shared_ptr<core::storage::IConnectionPool> db_pool;
        {
            const char* uri = std::getenv("DB_URI");
            if (uri && *uri) {
                corelog::info(std::string("Detected DB_URI: ") + uri);
                core::storage::PoolOptions popts{};
                if (const char* v = std::getenv("DB_POOL_MIN")) popts.min_size = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_POOL_MAX")) popts.max_size = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_CONN_TIMEOUT_MS")) popts.connect_timeout_ms = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_QUERY_TIMEOUT_MS")) popts.query_timeout_ms = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_PREPARE_STATEMENTS")) popts.prepare_statements = (std::strcmp(v, "0") != 0);

                db_pool = server::storage::postgres::make_connection_pool(uri, popts);
                if (!db_pool || !db_pool->health_check()) {
                    corelog::error("DB health check failed; please verify DB_URI.");
                    return 2;
                }
                corelog::info("DB connection pool initialised.");
            } else {
                corelog::warn("DB_URI is not set; database features remain disabled (configure if required).");
            }
        }
        if (db_pool) {
            services::set(db_pool);
            std::size_t configured_workers = 0;
            if (const char* env_workers = std::getenv("DB_WORKER_THREADS"); env_workers && *env_workers) {
                configured_workers = std::strtoul(env_workers, nullptr, 10);
            }
            std::size_t log_workers = configured_workers;
            if (log_workers == 0) {
                log_workers = std::thread::hardware_concurrency();
                if (log_workers == 0) log_workers = 1;
            }
            db_workers = std::make_shared<core::storage::DbWorkerPool>(db_pool);
            db_workers->start(configured_workers);
            services::set(db_workers);
            corelog::info(std::string("DB worker pool started: ") + std::to_string(log_workers) + " threads.");

            // 주기적인 DB 헬스 체크 스케줄링
            scheduler.schedule_every([job_queue_ptr, db_pool]() {
                job_queue_ptr->Push([db_pool]() {
                    try {
                        if (!db_pool->health_check()) {
                            corelog::warn("Periodic DB health check failed");
                        }
                    } catch (const std::exception& ex) {
                        corelog::error(std::string("Periodic DB health check exception: ") + ex.what());
                    } catch (...) {
                        corelog::error("Periodic DB health check unknown exception");
                    }
                });
            }, std::chrono::seconds(60));
        }

        // 6. Redis 클라이언트 구성 (환경 변수 기반)
        std::shared_ptr<server::storage::redis::IRedisClient> redis;
        if (const char* ruri = std::getenv("REDIS_URI"); ruri && *ruri) {
            corelog::info(std::string("Detected REDIS_URI: ") + ruri);
            server::storage::redis::Options ropts{};
            if (const char* v = std::getenv("REDIS_POOL_MAX")) ropts.pool_max = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
            if (const char* v = std::getenv("REDIS_USE_STREAMS")) ropts.use_streams = (std::strcmp(v, "0") != 0);
            redis = server::storage::redis::make_redis_client(ruri, ropts);
            
            // 개발 환경 또는 단일 게이트웨이 환경에서 시작 시 이전 Presence 데이터를 정리
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
                corelog::error("Redis health check failed; please verify REDIS_URI.");
            } else {
                corelog::info("Redis client initialised.");
            }
        } else {
            corelog::warn("REDIS_URI is not set; Redis features remain disabled (configure if required).");
        }
        if (redis) {
            services::set(redis);
            
            // 주기적인 Redis 헬스 체크
            scheduler.schedule_every([job_queue_ptr, redis]() {
                job_queue_ptr->Push([redis]() {
                    try {
                        if (!redis->health_check()) {
                            corelog::warn("Periodic Redis health check failed");
                        }
                    } catch (const std::exception& ex) {
                        corelog::error(std::string("Periodic Redis health check exception: ") + ex.what());
                    } catch (...) {
                        corelog::error("Periodic Redis health check unknown exception");
                    }
                });
            }, std::chrono::seconds(60));
            
            // 인스턴스 레지스트리에 서버 등록
            try {
                auto registry_client = server::state::make_redis_state_client(redis);
                registry_backend = std::make_shared<server::state::RedisInstanceStateBackend>(registry_client, registry_prefix, registry_ttl);
                registry_record.instance_id = server_instance_id;
                registry_record.host = advertise_host;
                registry_record.port = advertise_port;
                registry_record.role = "server";
                registry_record.capacity = 0;
                registry_record.active_sessions = 0;
                registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                if (registry_backend->upsert(registry_record)) {
                    registry_registered = true;
                    corelog::info("Registered server instance id=" + registry_record.instance_id + " host=" + registry_record.host + ":" + std::to_string(registry_record.port));
                } else {
                    corelog::warn("Failed to register server instance in registry");
                }
            } catch (const std::exception& ex) {
                corelog::warn(std::string("Failed to initialise server registry backend: ") + ex.what());
            }
            
            // 레지스트리 하트비트 스케줄링
            if (registry_registered) {
                scheduler.schedule_every([registry_backend, registry_record, registry_heartbeat_interval]() mutable {
                    registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (!registry_backend->upsert(registry_record)) {
                        corelog::warn("Server registry heartbeat upsert failed");
                    }
                }, registry_heartbeat_interval);
            }
        }

        // 7. 채팅 서비스 및 라우터 초기화
        server::app::chat::ChatService chat(io, job_queue, db_pool, redis);
        services::set(make_ref(chat));
        
        // Dispatcher에 핸들러 등록
        register_routes(dispatcher, chat);

        // 8. TCP 리스너 시작
        tcp::endpoint ep(tcp::v4(), port);
        auto acceptor = std::make_shared<core::Acceptor>(io, ep, dispatcher, buffer_manager, options, state,
            [&chat](std::shared_ptr<core::Session> sess){
                // 세션 종료 시 ChatService에 알림
                sess->set_on_close([&chat](std::shared_ptr<core::Session> s){ chat.on_session_close(s); });
            });
        services::set(acceptor);
        acceptor->start();
        corelog::info("server_app listening on 0.0.0.0:" + std::to_string(port));

        // 9. 워커 스레드 풀 시작
        unsigned int num_worker_threads = std::max(1u, std::thread::hardware_concurrency());
        workers.Start(num_worker_threads);
        corelog::info(std::to_string(num_worker_threads) + " worker threads started.");

        // 10. I/O 스레드 풀 시작 (Boost.Asio io_context 실행)
        unsigned int num_io_threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> io_threads;
        io_threads.reserve(num_io_threads);
        for (unsigned int i = 0; i < num_io_threads; ++i) {
            io_threads.emplace_back([&io]() { 
                try {
                    io.run();
                } catch (const std::exception& e) {
                    corelog::error(std::string("I/O thread exception: ") + e.what());
                }
            });
        }
        corelog::info(std::to_string(num_io_threads) + " I/O threads started.");

        // 11. Redis Pub/Sub 구독 (분산 채팅용)
        if (redis) {
            if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
                std::string prefix; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) prefix = p;
                std::string pattern = prefix + std::string("fanout:room:*");
                std::string gwid = [](){ const char* g = std::getenv("GATEWAY_ID"); if (g && *g) return std::string(g); return std::string("gw-default"); }();
                redis->start_psubscribe(pattern, [&chat, gwid](const std::string& channel, const std::string& message){
                    if (message.rfind("gw=", 0) == 0) {
                        auto nl = message.find('\n'); if (nl == std::string::npos) return;
                        std::string from = message.substr(3, nl - 3);
                        if (from == gwid) { // 자신이 보낸 메시지는 무시 (Self-Echo 방지)
                            auto d = ++g_self_echo_drop_total;
                            corelog::info(std::string("metric=self_echo_drop_total value=") + std::to_string(d));
                            return;
                        }
                        std::string payload = message.substr(nl + 1);
                        std::string room = channel; auto pos = room.rfind(':'); if (pos != std::string::npos) room = room.substr(pos + 1);
                        std::vector<std::uint8_t> body(payload.begin(), payload.end());
                        // 구독 지연 시간 측정 (메트릭)
                        try {
                            server::wire::v1::ChatBroadcast pb;
                            if (pb.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
                                if (pb.ts_ms() > 0 && now_ms >= static_cast<long long>(pb.ts_ms())) {
                                    auto lag = static_cast<long long>(now_ms - static_cast<long long>(pb.ts_ms()));
                                    g_subscribe_last_lag_ms.store(lag, std::memory_order_relaxed);
                                    corelog::info(std::string("metric=subscribe_lag_ms value=") + std::to_string(lag) + " room=" + room);
                                }
                            }
                        } catch (...) {}
                        chat.broadcast_room(room, body, nullptr);
                        auto n = ++g_subscribe_total;
                        corelog::info(std::string("metric=subscribe_total value=") + std::to_string(n) + " room=" + room);
                    }
                });
                corelog::info(std::string("Subscribed Redis pattern: ") + pattern);
            }
        }

        // Metrics HTTP 엔드포인트(METRICS_PORT 설정 시 활성화)
        std::unique_ptr<std::thread> metrics_thread;
        std::shared_ptr<asio::io_context> metrics_io;
        std::shared_ptr<tcp::acceptor> metrics_acc;
        unsigned short metrics_port = 0;
        if (const char* mp = std::getenv("METRICS_PORT"); mp && *mp) {
            unsigned long v = std::strtoul(mp, nullptr, 10); if (v > 0 && v < 65536) metrics_port = static_cast<unsigned short>(v);
        }
        if (metrics_port != 0) {
            metrics_io = std::make_shared<asio::io_context>();
            metrics_acc = std::make_shared<tcp::acceptor>(*metrics_io, tcp::endpoint(tcp::v4(), metrics_port));
            services::set(metrics_io);
            services::set(metrics_acc);
            auto do_accept = std::make_shared<std::function<void()>>();
            *do_accept = [metrics_io, metrics_acc, do_accept]() {
                auto sock = std::make_shared<tcp::socket>(*metrics_io);
                metrics_acc->async_accept(*sock, [metrics_io, metrics_acc, sock, do_accept](const boost::system::error_code& ec){
                    if (!ec) {
                        asio::post(metrics_io->get_executor(), [sock]() {
                            try {
                                asio::streambuf request_buf;
                                boost::system::error_code read_ec;
                                asio::read_until(*sock, request_buf, "\r\n\r\n", read_ec);
                                if (read_ec && read_ec != asio::error::eof) {
                                    boost::system::error_code ec;
                                    sock->close(ec);
                                    return;
                                }

                                std::istream request_stream(&request_buf);
                                std::string request_line;
                                std::getline(request_stream, request_line);
                                if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

                                std::string method;
                                std::string target;
                                {
                                    std::istringstream line_stream(request_line);
                                    line_stream >> method >> target;
                                }
                                if (target.empty()) target = "/metrics";

                                std::string body;
                                std::string status = "200 OK";
                                std::string content_type = "text/plain; version=0.0.4";

                                if (target == "/logs") {
                                    content_type = "text/plain; charset=utf-8";
                                    auto logs = corelog::recent(200);
                                    std::ostringstream body_stream;
                                    if (logs.empty()) {
                                        body_stream << "(no log entries)\n";
                                    } else {
                                        for (const auto& line : logs) {
                                            body_stream << line << '\n';
                                        }
                                    }
                                    body = body_stream.str();
                                } else if (target == "/metrics" || target == "/") {
                                    auto snap = server::core::runtime_metrics::snapshot();
                                    std::ostringstream stream;
                                    auto append_counter = [&](const char* name, std::uint64_t value) {
                                        stream << "# TYPE " << name << " counter\n" << name << ' ' << value << '\n';
                                    };
                                    auto append_gauge = [&](const char* name, long double value) {
                                        stream << "# TYPE " << name << " gauge\n" << name << ' ' << std::fixed << std::setprecision(3) << value << '\n';
                                        stream << std::defaultfloat << std::setprecision(6);
                                    };

                                    append_counter("chat_subscribe_total", g_subscribe_total.load());
                                    append_counter("chat_self_echo_drop_total", g_self_echo_drop_total.load());
                                    append_gauge("chat_subscribe_last_lag_ms", static_cast<long double>(g_subscribe_last_lag_ms.load()));

                                    append_counter("chat_accept_total", snap.accept_total);
                                    append_counter("chat_session_started_total", snap.session_started_total);
                                    append_counter("chat_session_stopped_total", snap.session_stopped_total);
                                    append_counter("chat_session_timeout_total", snap.session_timeout_total);
                                    append_counter("chat_heartbeat_timeout_total", snap.heartbeat_timeout_total);
                                    append_counter("chat_send_queue_drop_total", snap.send_queue_drop_total);
                                    append_gauge("chat_session_active", static_cast<long double>(snap.session_active));
                                    append_counter("chat_frame_total", snap.frame_total);
                                    append_counter("chat_frame_error_total", snap.frame_error_total);
                                    append_counter("chat_frame_payload_sum_bytes", snap.frame_payload_sum_bytes);
                                    append_counter("chat_frame_payload_count", snap.frame_payload_count);
                                    auto payload_avg = snap.frame_payload_count ? (static_cast<long double>(snap.frame_payload_sum_bytes) / static_cast<long double>(snap.frame_payload_count)) : 0.0L;
                                    append_gauge("chat_frame_payload_avg_bytes", payload_avg);
                                    append_gauge("chat_frame_payload_max_bytes", static_cast<long double>(snap.frame_payload_max_bytes));
                                    append_counter("chat_dispatch_total", snap.dispatch_total);
                                    append_counter("chat_dispatch_unknown_total", snap.dispatch_unknown_total);
                                    append_counter("chat_dispatch_exception_total", snap.dispatch_exception_total);

                                    auto last_ms = static_cast<long double>(snap.dispatch_latency_last_ns) / 1'000'000.0L;
                                    auto max_ms = static_cast<long double>(snap.dispatch_latency_max_ns) / 1'000'000.0L;
                                    auto sum_ms = static_cast<long double>(snap.dispatch_latency_sum_ns) / 1'000'000.0L;
                                    auto avg_ms = snap.dispatch_latency_count ? (sum_ms / static_cast<long double>(snap.dispatch_latency_count)) : 0.0L;
                                    append_gauge("chat_dispatch_last_latency_ms", last_ms);
                                    append_gauge("chat_dispatch_max_latency_ms", max_ms);
                                    append_gauge("chat_dispatch_latency_sum_ms", sum_ms);
                                    append_gauge("chat_dispatch_latency_avg_ms", avg_ms);
                                    append_counter("chat_dispatch_latency_count", snap.dispatch_latency_count);
                                append_gauge("chat_job_queue_depth", static_cast<long double>(snap.job_queue_depth));
                                append_gauge("chat_job_queue_depth_peak", static_cast<long double>(snap.job_queue_depth_peak));
                                append_gauge("chat_db_job_queue_depth", static_cast<long double>(snap.db_job_queue_depth));
                                append_gauge("chat_db_job_queue_depth_peak", static_cast<long double>(snap.db_job_queue_depth_peak));
                                append_counter("chat_db_job_processed_total", snap.db_job_processed_total);
                                append_counter("chat_db_job_failed_total", snap.db_job_failed_total);
                                append_gauge("chat_memory_pool_capacity", static_cast<long double>(snap.memory_pool_capacity));
                                    append_gauge("chat_memory_pool_in_use", static_cast<long double>(snap.memory_pool_in_use));
                                    append_gauge("chat_memory_pool_in_use_peak", static_cast<long double>(snap.memory_pool_in_use_peak));

                                    if (!snap.opcode_counts.empty()) {
                                        stream << "# TYPE chat_dispatch_opcode_total counter\n";
                                        for (const auto& [opcode, count] : snap.opcode_counts) {
                                            std::ostringstream label;
                                            label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
                                            stream << "chat_dispatch_opcode_total{opcode=\"0x" << label.str() << "\"} " << count << "\n";
                                        }
                                    }

                                    stream << std::setfill(' ') << std::dec << std::nouppercase;
                                    body = stream.str();
                                } else {
                                    status = "404 Not Found";
                                    content_type = "text/plain; charset=utf-8";
                                    body = "Not Found\r\n";
                                }

                                std::string hdr = "HTTP/1.1 " + status + "\r\nContent-Type: " + content_type +
                                    "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                                std::vector<asio::const_buffer> bufs;
                                bufs.emplace_back(asio::buffer(hdr));
                                if (!body.empty()) {
                                    bufs.emplace_back(asio::buffer(body));
                                }
                                asio::write(*sock, bufs);
                                boost::system::error_code ec;
                                sock->shutdown(tcp::socket::shutdown_both, ec);
                                if (ec) ec.clear();
                                sock->close(ec);
                            } catch (...) {}
                        });
                    }
                    // 다음 accept
                    asio::post(metrics_io->get_executor(), *do_accept);
                });
            };
            (*do_accept)();
            metrics_thread = std::make_unique<std::thread>([metrics_io](){
                try { metrics_io->run(); } catch (...) {}
            });
            corelog::info(std::string("Metrics listening on :") + std::to_string(metrics_port));
        }
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code&, int) {
            corelog::info("Shutdown signal received...");
            if (registry_registered && registry_backend) {
                try {
                    registry_backend->remove(registry_record.instance_id);
                } catch (const std::exception& ex) {
                    corelog::warn(std::string("Failed to remove server registry entry during shutdown: ") + ex.what());
                } catch (...) {
                    corelog::warn("Failed to remove server registry entry during shutdown");
                }
                registry_registered = false;
            }
            // Redis Pub/Sub 구독 중지(있다면)
            try { if (redis) { redis->stop_psubscribe(); } } catch (...) {}
            if (metrics_io) { try { metrics_io->stop(); } catch (...) {} }
            scheduler.shutdown();
            try { scheduler_timer->cancel(); } catch (...) {}
            if (db_workers) { try { db_workers->stop(); } catch (...) {} }
            acceptor->stop();
            io.stop();
            workers.Stop();
        });

        for (auto& t : io_threads) {
            t.join();
        }

        if (registry_registered && registry_backend) {
            try {
                registry_backend->remove(registry_record.instance_id);
            } catch (...) {
                corelog::warn("Failed to remove server registry entry on shutdown");
            }
            registry_registered = false;
        }
        if (metrics_thread && metrics_thread->joinable()) metrics_thread->join();
        try { scheduler_timer->cancel(); } catch (...) {}
        scheduler.shutdown();
        if (db_workers) db_workers->stop();
        services::clear();
        return 0;
    } catch (const std::exception& ex) {
        corelog::error(std::string("server_app exception: ") + ex.what());
        try { scheduler_timer->cancel(); } catch (...) {}
        scheduler.shutdown();
        if (db_workers) db_workers->stop();
        services::clear();
        return 1;
    }
}

} // namespace server::app


















