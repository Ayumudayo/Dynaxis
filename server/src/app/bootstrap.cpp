#include <iostream>
#include <array>
#include <thread>
#include <vector>
#include <string>
#include <span>
#include <csignal>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <cctype>
#include <unordered_map>

#include <boost/asio.hpp>
#include <clocale>
#if defined(_WIN32)
#  include <windows.h>
#endif

#include "server/app/bootstrap.hpp"
#include "server/app/router.hpp"
#include "server/app/config.hpp"
#include "server/app/metrics_server.hpp"

#include "server/core/net/acceptor.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/net/session.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/service_registry.hpp"
#include "server/core/util/crash_handler.hpp"
#include "server/core/app/app_host.hpp"
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

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace core = server::core;
namespace corelog = server::core::log;
namespace services = server::core::util::services;
namespace crash = server::core::util::crash;

namespace server::app {

/**
 * @brief server_app 부트스트랩(설정/DI/리스너/스케줄러) 구현입니다.
 *
 * 프로세스 시작 시 의존성 상태를 단계적으로 올리고,
 * 종료 시에는 등록된 shutdown 단계를 역순으로 실행해 자원 해제 순서를 안정화합니다.
 */
// 전역 메트릭 변수 (metrics_server.cpp에서 참조)
std::atomic<std::uint64_t> g_subscribe_total{0};
std::atomic<std::uint64_t> g_self_echo_drop_total{0};
std::atomic<long long>     g_subscribe_last_lag_ms{0};

namespace {

std::string trim_ascii(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::unordered_map<std::string, std::string> parse_kv_lines(std::string_view payload) {
    std::unordered_map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= payload.size()) {
        std::size_t end = payload.find('\n', start);
        if (end == std::string_view::npos) {
            end = payload.size();
        }

        const auto line = payload.substr(start, end - start);
        const auto eq = line.find('=');
        if (eq != std::string_view::npos) {
            const std::string key = trim_ascii(line.substr(0, eq));
            const std::string value = trim_ascii(line.substr(eq + 1));
            if (!key.empty()) {
                out[key] = value;
            }
        }

        if (end == payload.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::vector<std::string> split_csv(std::string_view input) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= input.size()) {
        std::size_t end = input.find(',', start);
        if (end == std::string_view::npos) {
            end = input.size();
        }

        std::string token = trim_ascii(input.substr(start, end - start));
        if (!token.empty()) {
            out.push_back(std::move(token));
        }

        if (end == input.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

} // namespace

// 메인 서버 실행 함수
// 1. 설정 로드
// 2. 핵심 컴포넌트(스레드 풀, I/O 컨텍스트) 초기화
// 3. 의존성 주입 (ServiceRegistry)
// 4. DB/Redis 연결 설정
// 5. 서버 인스턴스 등록 (Service Discovery)
// 6. TCP 리스너 시작
/**
 * @brief 서버 프로세스를 기동합니다.
 * @param argc 커맨드라인 인자 개수
 * @param argv 커맨드라인 인자 배열
 * @return 종료 코드(0 정상)
 */
int run_server(int argc, char** argv) {
    // 1. 핵심 컴포넌트 선언
    core::concurrent::TaskScheduler scheduler;
    std::shared_ptr<asio::steady_timer> scheduler_timer;
    std::shared_ptr<core::storage::DbWorkerPool> db_workers;
    std::shared_ptr<server::state::RedisInstanceStateBackend> registry_backend;
    server::state::InstanceRecord registry_record{};
    bool registry_registered = false;
    server::core::app::AppHost app_host{"server_app"};

    try {
#if defined(_WIN32)
        // 윈도우 콘솔의 인코딩을 UTF-8로 설정
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        // 크래시 핸들러 설치
        crash::install();

        // 2. 설정 로드
        ServerConfig config;
        if (!config.load(argc, argv)) {
            corelog::error("Failed to load configuration");
            return 1;
        }

        // Readiness: DB is required; Redis is required when configured.
        app_host.declare_dependency("db");
        if (!config.redis_uri.empty()) {
            app_host.declare_dependency("redis");
        }

        // Base readiness will flip to true after listeners/threads are running.
        app_host.set_ready(false);

        if (config.log_buffer_capacity > 0) {
            corelog::set_buffer_capacity(config.log_buffer_capacity);
            corelog::info(std::string("Log buffer capacity set to ") + std::to_string(config.log_buffer_capacity));
        }

        // 3. 코어 컴포넌트 초기화
        asio::io_context io;
        core::JobQueue job_queue(config.job_queue_max);
        auto* job_queue_ptr = &job_queue;
        core::ThreadManager workers(job_queue);
        core::BufferManager buffer_manager(2048, 1024);

        core::Dispatcher dispatcher;
        auto options = std::make_shared<core::SessionOptions>();
        options->read_timeout_ms = 60'000;
        options->heartbeat_interval_ms = 10'000;
        auto state = std::make_shared<core::SharedState>();

        // ServiceRegistry 등록
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
        services::set(make_ref(app_host));

        // 스케줄러 타이머 설정
        scheduler_timer = std::make_shared<asio::steady_timer>(io);
        services::set(scheduler_timer);

        auto scheduler_tick = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> scheduler_tick_weak = scheduler_tick;
        *scheduler_tick = [scheduler_timer, scheduler_tick_weak, scheduler_ptr = &scheduler]() {
            scheduler_ptr->poll(32);
            scheduler_timer->expires_after(std::chrono::milliseconds(50));
            scheduler_timer->async_wait([scheduler_timer, scheduler_tick_weak, scheduler_ptr](const boost::system::error_code& ec) {
                if (ec == asio::error::operation_aborted) return;
                if (!ec) {
                    scheduler_ptr->poll(32);
                    if (auto locked = scheduler_tick_weak.lock()) (*locked)();
                }
            });
        };
        (*scheduler_tick)();

        // 4. DB 커넥션 풀 구성
        std::shared_ptr<core::storage::IConnectionPool> db_pool;
        if (!config.db_uri.empty()) {
            corelog::info("Detected DB_URI (redacted)");
            core::storage::PoolOptions popts{};
            popts.min_size = config.db_pool_min;
            popts.max_size = config.db_pool_max;
            popts.connect_timeout_ms = config.db_conn_timeout_ms;
            popts.query_timeout_ms = config.db_query_timeout_ms;
            popts.prepare_statements = config.db_prepare_statements;

            db_pool = server::storage::postgres::make_connection_pool(config.db_uri.c_str(), popts);
            if (!db_pool || !db_pool->health_check()) {
                corelog::error("DB health check failed; please verify DB_URI.");
                return 2;
            }
            corelog::info("DB connection pool initialised.");
            app_host.set_dependency_ok("db", true);
        } else {
            corelog::warn("DB_URI is not set; database features remain disabled.");
            app_host.set_dependency_ok("db", false);
        }

        if (db_pool) {
            services::set(db_pool);
            std::size_t log_workers = config.db_worker_threads;
            if (log_workers == 0) {
                log_workers = std::thread::hardware_concurrency();
                if (log_workers == 0) log_workers = 1;
            }
            db_workers = std::make_shared<core::storage::DbWorkerPool>(db_pool, config.db_job_queue_max);
            db_workers->start(config.db_worker_threads);
            services::set(db_workers);
            corelog::info(std::string("DB worker pool started: ") + std::to_string(log_workers) + " threads.");

            // 주기적인 DB 헬스 체크
            scheduler.schedule_every([job_queue_ptr, db_pool, &app_host]() {
                job_queue_ptr->Push([db_pool, &app_host]() {
                    try {
                        const bool ok = db_pool->health_check();
                        if (!ok) {
                            corelog::warn("Periodic DB health check failed");
                        }
                        app_host.set_dependency_ok("db", ok);
                    } catch (const std::exception& ex) {
                        corelog::error(std::string("Periodic DB health check exception: ") + ex.what());
                    } catch (...) {
                        corelog::error("Periodic DB health check unknown exception");
                    }
                });
            }, std::chrono::seconds(60));
        }

        // 5. Redis 클라이언트 구성
        std::shared_ptr<server::storage::redis::IRedisClient> redis;
        if (!config.redis_uri.empty()) {
            corelog::info("Detected REDIS_URI (redacted)");
            server::storage::redis::Options ropts{};
            ropts.pool_max = config.redis_pool_max;
            ropts.use_streams = config.redis_use_streams;
            redis = server::storage::redis::make_redis_client(config.redis_uri.c_str(), ropts);

            if (redis) {
                if (config.presence_clean_on_start) {
                    std::string pattern = config.redis_channel_prefix + std::string("presence:room:*");
                    corelog::warn(std::string("Presence cleanup on start: ") + pattern);
                    (void)redis->scan_del(pattern);
                }
            }
            const bool ok = (redis && redis->health_check());
            if (!ok) {
                corelog::error("Redis health check failed; please verify REDIS_URI.");
            } else {
                corelog::info("Redis client initialised.");
            }
            app_host.set_dependency_ok("redis", ok);
        } else {
            corelog::warn("REDIS_URI is not set; Redis features remain disabled.");
        }

        if (redis) {
            services::set(redis);

            // 주기적인 Redis 헬스 체크
            scheduler.schedule_every([job_queue_ptr, redis, &app_host]() {
                job_queue_ptr->Push([redis, &app_host]() {
                    try {
                        const bool ok = redis->health_check();
                        if (!ok) {
                            corelog::warn("Periodic Redis health check failed");
                        }
                        app_host.set_dependency_ok("redis", ok);
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
                registry_backend = std::make_shared<server::state::RedisInstanceStateBackend>(
                    registry_client, config.registry_prefix, config.registry_ttl);
                
                registry_record.instance_id = config.server_instance_id;
                registry_record.host = config.advertise_host;
                registry_record.port = config.advertise_port;
                registry_record.role = "server";
                registry_record.capacity = 0;
                registry_record.active_sessions = 0;
                registry_record.ready = app_host.ready();
                registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

                if (registry_backend->upsert(registry_record)) {
                    registry_registered = true;
                    corelog::info("Registered server instance id=" + registry_record.instance_id + 
                                  " host=" + registry_record.host + ":" + std::to_string(registry_record.port));
                } else {
                    corelog::warn("Failed to register server instance in registry");
                }
            } catch (const std::exception& ex) {
                corelog::warn(std::string("Failed to initialise server registry backend: ") + ex.what());
            }

            // 레지스트리 하트비트 스케줄링
            if (registry_registered) {
                auto interval = config.registry_heartbeat_interval;
                scheduler.schedule_every([registry_backend, registry_record, state, &app_host]() mutable {
                    registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                    registry_record.active_sessions = state->connection_count.load(std::memory_order_relaxed);
                    registry_record.ready = app_host.ready();
                    if (!registry_backend->upsert(registry_record)) {
                        corelog::warn("Server registry heartbeat upsert failed");
                    }
                }, interval);
            }
        }

        // 6. 채팅 서비스 및 라우터 초기화
        server::app::chat::ChatService chat(io, job_queue, db_pool, redis);
        services::set(make_ref(chat));
        register_routes(dispatcher, chat);

        // 7. TCP 리스너 시작
        tcp::endpoint ep(tcp::v4(), config.port);
        auto acceptor = std::make_shared<core::net::SessionListener>(io, ep, dispatcher, buffer_manager, options, state,
            [&chat](std::shared_ptr<core::net::Session> sess){
                sess->set_on_close([&chat](std::shared_ptr<core::net::Session> s){ chat.on_session_close(s); });
            });
        services::set(acceptor);
        acceptor->start();
        corelog::info("server_app listening on 0.0.0.0:" + std::to_string(config.port));

        // 8. 워커 스레드 풀 시작
        unsigned int num_worker_threads = std::max(1u, std::thread::hardware_concurrency());
        workers.Start(num_worker_threads);
        corelog::info(std::to_string(num_worker_threads) + " worker threads started.");

        // 9. I/O 스레드 풀 시작
        unsigned int num_io_threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> io_threads;
        io_threads.reserve(num_io_threads);
        for (unsigned int i = 0; i < num_io_threads; ++i) {
            io_threads.emplace_back([&io]() { 
                try { io.run(); } catch (const std::exception& e) {
                    corelog::error(std::string("I/O thread exception: ") + e.what());
                }
            });
        }
        corelog::info(std::to_string(num_io_threads) + " I/O threads started.");

        // Readiness is computed from base readiness + dependency probes.
        app_host.set_ready(true);

        if (registry_registered && registry_backend) {
            registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            registry_record.active_sessions = state->connection_count.load(std::memory_order_relaxed);
            registry_record.ready = app_host.ready();
            if (!registry_backend->upsert(registry_record)) {
                corelog::warn("Server registry ready-state upsert failed");
            }
        }

        // 10. Redis Pub/Sub 구독 (분산 채팅용)
        if (redis && config.use_redis_pubsub) {
            // 통합 패턴 구독 (RedisClientImpl이 단일 구독만 지원하므로)
            std::string pattern_all = config.redis_channel_prefix + std::string("fanout:*");
            std::string gwid = config.gateway_id;

            const std::string refresh_prefix = config.redis_channel_prefix + "fanout:refresh:";
            const std::string room_prefix = config.redis_channel_prefix + "fanout:room:";

            enum class ExactFanoutChannel {
                kWhisper,
                kAdminDisconnect,
                kAdminAnnounce,
                kAdminSettings,
            };

            using ExactChannelEntry = std::pair<std::string, ExactFanoutChannel>;
            const std::array<ExactChannelEntry, 4> exact_channels{{
                {config.redis_channel_prefix + "fanout:whisper", ExactFanoutChannel::kWhisper},
                {config.redis_channel_prefix + "fanout:admin:disconnect", ExactFanoutChannel::kAdminDisconnect},
                {config.redis_channel_prefix + "fanout:admin:announce", ExactFanoutChannel::kAdminAnnounce},
                {config.redis_channel_prefix + "fanout:admin:settings", ExactFanoutChannel::kAdminSettings},
            }};

            redis->start_psubscribe(pattern_all,
                                    [&chat,
                                     gwid,
                                     refresh_prefix,
                                     room_prefix,
                                     exact_channels](const std::string& channel, const std::string& message) {
                // 1. Self-Echo 방지
                if (message.rfind("gw=", 0) == 0) {
                    auto nl = message.find('\n');
                    std::string from;
                    if (nl != std::string::npos) {
                        from = message.substr(3, nl - 3);
                    } else {
                        from = message.substr(3);
                    }
                    if (from == gwid) {
                        // g_self_echo_drop_total++;
                        return;
                    }

                    // 2. 채널 분기 처리
                    // channel: prefix + "fanout:room:<room>" OR prefix + "fanout:refresh:<room>" OR prefix + "fanout:whisper"
                    if (channel.rfind(refresh_prefix, 0) == 0) {
                        // Refresh Notification
                        std::string room = channel.substr(refresh_prefix.size());
                        chat.broadcast_refresh_local(room);
                        corelog::info("DEBUG: Received refresh notify for room: " + room + " from " + from);
                        return;
                    }

                    if (channel.rfind(room_prefix, 0) == 0) {
                        // Chat Broadcast
                        if (nl == std::string::npos) {
                            return; // 채팅은 payload 필수
                        }
                        std::string payload = message.substr(nl + 1);
                        std::string room = channel.substr(room_prefix.size());
                        std::vector<std::uint8_t> body(payload.begin(), payload.end());
                        chat.broadcast_room(room, body, nullptr);
                        g_subscribe_total++;
                        return;
                    }

                    std::optional<ExactFanoutChannel> exact_match;
                    for (const auto& [name, kind] : exact_channels) {
                        if (channel == name) {
                            exact_match = kind;
                            break;
                        }
                    }

                    if (!exact_match.has_value()) {
                        return;
                    }

                    if (nl == std::string::npos) {
                        return;
                    }

                    const std::string payload = message.substr(nl + 1);
                    switch (*exact_match) {
                    case ExactFanoutChannel::kWhisper: {
                        std::vector<std::uint8_t> body(payload.begin(), payload.end());
                        chat.deliver_remote_whisper(body);
                        g_subscribe_total++;
                        return;
                    }
                    case ExactFanoutChannel::kAdminDisconnect: {
                        const auto fields = parse_kv_lines(payload);

                        auto it = fields.find("client_ids");
                        if (it == fields.end() || it->second.empty()) {
                            return;
                        }

                        auto users = split_csv(it->second);
                        if (users.empty()) {
                            return;
                        }

                        std::string reason = "Disconnected by administrator";
                        if (const auto reason_it = fields.find("reason"); reason_it != fields.end() && !reason_it->second.empty()) {
                            reason = reason_it->second;
                        }

                        chat.admin_disconnect_users(users, reason);
                        g_subscribe_total++;
                        return;
                    }
                    case ExactFanoutChannel::kAdminAnnounce: {
                        const auto fields = parse_kv_lines(payload);

                        const auto text_it = fields.find("text");
                        if (text_it == fields.end() || text_it->second.empty()) {
                            return;
                        }

                        chat.admin_broadcast_notice(text_it->second);
                        g_subscribe_total++;
                        return;
                    }
                    case ExactFanoutChannel::kAdminSettings: {
                        const auto fields = parse_kv_lines(payload);

                        const auto key_it = fields.find("key");
                        const auto value_it = fields.find("value");
                        if (key_it == fields.end() || value_it == fields.end() || key_it->second.empty() || value_it->second.empty()) {
                            return;
                        }

                        chat.admin_apply_runtime_setting(key_it->second, value_it->second);
                        g_subscribe_total++;
                        return;
                    }
                    }

                    return;
                }
            });

            corelog::info(std::string("Subscribed Redis pattern: ") + pattern_all);
        }

        // 11. Metrics Server 시작
        std::unique_ptr<MetricsServer> metrics_server;
        if (config.metrics_port != 0) {
            metrics_server = std::make_unique<MetricsServer>(config.metrics_port);
            metrics_server->start();
        }

        // 12. 종료 시그널 대기
        app_host.add_shutdown_step("stop workers", [&]() { workers.Stop(); });
        app_host.add_shutdown_step("stop io_context", [&]() { io.stop(); });
        app_host.add_shutdown_step("stop acceptor", [&]() { acceptor->stop(); });
        app_host.add_shutdown_step("stop db worker pool", [&]() {
            if (db_workers) {
                try { db_workers->stop(); } catch (...) {}
            }
        });
        app_host.add_shutdown_step("cancel scheduler timer", [&]() {
            try {
                if (scheduler_timer) scheduler_timer->cancel();
            } catch (...) {
            }
        });
        app_host.add_shutdown_step("shutdown scheduler", [&]() { scheduler.shutdown(); });
        app_host.add_shutdown_step("stop metrics server", [&]() {
            if (metrics_server) metrics_server->stop();
        });
        app_host.add_shutdown_step("stop redis pubsub", [&]() {
            try { if (redis) redis->stop_psubscribe(); } catch (...) {}
        });
        app_host.add_shutdown_step("deregister instance", [&]() {
            if (registry_registered && registry_backend) {
                try { registry_backend->remove(registry_record.instance_id); } catch (...) {}
                registry_registered = false;
            }
        });

        app_host.install_asio_termination_signals(io, {});

        for (auto& t : io_threads) {
            t.join();
        }

        app_host.set_ready(false);

        // 정리 작업
        if (registry_registered && registry_backend) {
            try { registry_backend->remove(registry_record.instance_id); } catch (...) {}
        }
        if (metrics_server) metrics_server->stop();
        try { scheduler_timer->cancel(); } catch (...) {}
        scheduler.shutdown();
        if (db_workers) db_workers->stop();
        services::clear();

        return 0;

    } catch (const std::exception& ex) {
        corelog::error(std::string("server_app exception: ") + ex.what());
        services::clear();
        return 1;
    }
}

} // namespace server::app
