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
#include <future>
#include <optional>
#include <cctype>
#include <unordered_map>
#include <limits>

#include <boost/asio.hpp>
#include <clocale>
#if defined(_WIN32)
#  include <windows.h>
#endif

#include "server/app/bootstrap.hpp"
#include "server/app/router.hpp"
#include "server/app/config.hpp"
#include "server/app/core_internal_adapter.hpp"
#include "server/app/metrics_server.hpp"
#include "admin_fanout_subscription.hpp"
#include "server_shutdown.hpp"
#include "server/app/topology_runtime_assignment.hpp"
#include "server/fps/fps_service.hpp"
#include "server_runtime_state.hpp"

#include "server/core/net/dispatcher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/service_registry.hpp"
#include "server/core/app/engine_builder.hpp"
#include "server/core/config/options.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/discovery/instance_registry.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/scripting/script_watcher.hpp"
#include "server/chat/chat_service.hpp"
// Protobuf (수신 payload ts_ms 파싱용)
#include "wire.pb.h"
// 캐시/팬아웃: Redis 클라이언트(스켈레톤)
#include "server/storage/connection_pool.hpp"

namespace asio = boost::asio;
namespace core = server::core;
namespace corelog = server::core::log;
namespace services = server::core::util::services;
namespace server::app {

/**
 * @brief server_app 부트스트랩(설정/DI/리스너/스케줄러) 구현입니다.
 *
 * 프로세스 시작 시 의존성 상태를 단계적으로 올리고,
 * 종료 시에는 등록된 shutdown 단계를 역순으로 실행해 자원 해제 순서를 안정화합니다.
 * 이 파일은 단순 초기화 나열이 아니라 "무엇을 언제 살아 있다고 간주하는가"를 정의하는 곳입니다.
 * 부트스트랩 순서가 흔들리면 ready 판정, Redis fanout 구독, 관리자 제어면, 종료 drain 동작이
 * 서로 다른 수명주기를 보게 되어 운영 중 재현하기 어려운 부팅/종료 버그가 생깁니다.
 */
namespace {

std::string make_lua_env_name(const std::filesystem::path& scripts_dir,
                              const std::filesystem::path& script_path) {
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(script_path, scripts_dir, ec);
    if (ec || relative.empty()) {
        relative = script_path.filename();
    }
    relative.replace_extension();

    std::string env_name = relative.lexically_normal().generic_string();
    if (env_name.empty()) {
        env_name = script_path.stem().string();
    }
    return env_name;
}

bool is_lua_script_file(const std::filesystem::path& path) {
    if (!path.has_extension()) {
        return false;
    }

    std::string ext = path.extension().string();
#if defined(_WIN32)
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return ext == ".lua";
}

std::vector<std::filesystem::path> list_lua_scripts(const std::filesystem::path& scripts_dir) {
    std::vector<std::filesystem::path> scripts;

    std::error_code ec;
    if (!std::filesystem::exists(scripts_dir, ec) || ec) {
        return scripts;
    }

    std::filesystem::recursive_directory_iterator it(scripts_dir, ec);
    if (ec) {
        return scripts;
    }

    for (const auto& entry : it) {
        std::error_code st_ec;
        if (!entry.is_regular_file(st_ec) || st_ec) {
            continue;
        }

        const auto path = entry.path();
        if (is_lua_script_file(path)) {
            scripts.push_back(path);
        }
    }

    std::sort(scripts.begin(), scripts.end(), [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
        return lhs.lexically_normal().generic_string() < rhs.lexically_normal().generic_string();
    });
    return scripts;
}

} // namespace

// 메인 서버 실행 함수는 "실행 순서" 자체가 계약이다.
// 설정, 의존성, 외부 저장소, 리스너, readiness를 아무 순서로나 켜면
// 포트는 열렸는데 아직 처리 워커가 없거나, ready는 열렸는데 fanout 구독은 늦는 식의 어색한 중간 상태가 생긴다.
/**
 * @brief 서버 프로세스를 기동합니다.
 * @param argc 커맨드라인 인자 개수
 * @param argv 커맨드라인 인자 배열
 * @return 종료 코드(0 정상)
 */
int run_server(int argc, char** argv) {
    // 1. 핵심 컴포넌트 선언
    // 선언만 먼저 하고 실제 ready는 나중에 올린다. 그래야 "객체는 생겼지만 아직 서비스 가능하지 않다"를 표현할 수 있다.
    core::concurrent::TaskScheduler scheduler;
    const auto bootstrap_task_group = scheduler.create_cancel_group();
    std::shared_ptr<asio::steady_timer> scheduler_timer;
    std::shared_ptr<std::function<void()>> scheduler_tick;
    std::shared_ptr<core::concurrent::TaskScheduler::Clock::time_point> scheduler_last_poll_at;
    std::shared_ptr<asio::steady_timer> fps_tick_timer;
    std::shared_ptr<core::storage_execution::DbWorkerPool> db_workers;
    std::shared_ptr<core::scripting::LuaRuntime> lua_runtime;
    std::shared_ptr<asio::strand<asio::io_context::executor_type>> lua_reload_strand;
    std::shared_ptr<core::scripting::ScriptWatcher> lua_script_watcher;
    std::shared_ptr<server::core::state::IInstanceStateBackend> registry_backend;
    server::core::state::InstanceRecord registry_record{};
    bool registry_registered = false;
    auto engine = server::core::app::EngineBuilder("server_app").build();
    auto& app_host = engine.host();
    core::runtime_metrics::set_liveness_state(core::runtime_metrics::LivenessState::kBootstrapping);

    try {
#if defined(_WIN32)
        // 윈도우 콘솔의 인코딩을 UTF-8로 설정
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        // 크래시 핸들러 설치
        core_internal::install_crash_handler();

        // 2. 설정 로드
        ServerConfig config;
        if (!config.load(argc, argv)) {
            corelog::error("Failed to load configuration");
            core::runtime_metrics::set_liveness_state(core::runtime_metrics::LivenessState::kFailed);
            engine.mark_failed();
            return 1;
        }

        reset_bootstrap_shutdown_drain_metrics(config.shutdown_drain_timeout_ms);

        // readiness는 "필수 의존성이 실제로 준비됐는가"를 뜻한다.
        // DB는 항상 필수이고, Redis는 구성된 경우에만 필수로 선언해 환경별 의미를 고정한다.
        engine.declare_dependency("db");
        if (!config.redis_uri.empty()) {
            engine.declare_dependency("redis");
        }

        // 기본 ready는 리스너와 워커가 실제로 올라온 뒤에만 true로 바뀐다.
        // 의존성만 붙었다고 먼저 ready를 열면 연결은 받지만 처리 스레드가 아직 없는 상태가 생길 수 있다.
        engine.set_ready(false);

        if (config.log_buffer_capacity > 0) {
            corelog::set_buffer_capacity(config.log_buffer_capacity);
            corelog::info(std::string("Log buffer capacity set to ") + std::to_string(config.log_buffer_capacity));
        }

        if (config.lua_enabled) {
            corelog::info(
                "Lua scripting enabled"
                " scripts_dir=" + config.lua_scripts_dir
                + " fallback_scripts_dir=" + config.lua_fallback_scripts_dir
                + " reload_interval_ms=" + std::to_string(config.lua_reload_interval_ms)
                + " instruction_limit=" + std::to_string(config.lua_instruction_limit)
                + " memory_limit_bytes=" + std::to_string(config.lua_memory_limit_bytes)
                + " auto_disable_threshold=" + std::to_string(config.lua_auto_disable_threshold));
            if (config.lua_scripts_dir.empty() && config.lua_fallback_scripts_dir.empty()) {
                corelog::warn("LUA_ENABLED is set but LUA_SCRIPTS_DIR and LUA_FALLBACK_SCRIPTS_DIR are both empty; scripts will not be loaded until configured");
            }
        }

        if (config.lua_enabled) {
            core::scripting::LuaRuntime::Config lua_cfg{};
            lua_cfg.instruction_limit = config.lua_instruction_limit;
            const std::uint64_t max_size_t = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
            const std::uint64_t capped_memory_limit =
                config.lua_memory_limit_bytes > max_size_t ? max_size_t : config.lua_memory_limit_bytes;
            lua_cfg.memory_limit_bytes = static_cast<std::size_t>(capped_memory_limit);

            lua_runtime = std::make_shared<core::scripting::LuaRuntime>(std::move(lua_cfg));
            engine.bridge_service(lua_runtime);
            corelog::info("Lua runtime scaffold initialised");
        }

        // 3. 코어 컴포넌트 초기화
        // 작업 큐, 스레드 풀, 스케줄러를 먼저 세워 두어야 이후 저장소/리스너가 올리는 후속 작업을 안전하게 받을 수 있다.
        asio::io_context io;
        if (lua_runtime) {
            lua_reload_strand = std::make_shared<asio::strand<asio::io_context::executor_type>>(
                asio::make_strand(io));
        }
        core::JobQueue job_queue(config.job_queue_max);
        auto* job_queue_ptr = &job_queue;
        core::ThreadManager workers(job_queue);
        core::Dispatcher dispatcher;
        auto options = std::make_shared<core::SessionOptions>();
        options->read_timeout_ms = 60'000;
        options->heartbeat_interval_ms = 10'000;
        const std::size_t buffer_block_size = std::max<std::size_t>(
            2048,
            server::core::protocol::k_header_bytes + options->recv_max_payload);
        core::BufferManager buffer_manager(buffer_block_size, 1024);
        auto state = core_internal::make_connection_runtime_state();

        // 현재 서버 런타임 그래프에 대한 compatibility bridge를 유지한다.
        // 기존 전역 lookup 사용처를 한 번에 없애기 전까지는, 이 다리가 있어야 새 runtime 소유권 모델과 공존할 수 있다.
        engine.bridge_alias(io);
        engine.bridge_alias(job_queue);
        engine.bridge_alias(workers);
        engine.bridge_alias(buffer_manager);
        engine.bridge_alias(dispatcher);
        engine.bridge_service(options);
        core_internal::register_connection_runtime_state_service(state);
        engine.bridge_alias(scheduler);
        engine.bridge_alias(app_host);
        engine.bridge_alias(engine);
        if (lua_reload_strand) {
            engine.bridge_service(lua_reload_strand);
        }

        // 스케줄러 타이머 설정
        engine.register_module(
            "scheduler-poll-loop",
            [&](server::core::app::EngineRuntime&) {
                scheduler_timer = std::make_shared<asio::steady_timer>(io);
                engine.bridge_service(scheduler_timer);
                scheduler_last_poll_at =
                    std::make_shared<core::concurrent::TaskScheduler::Clock::time_point>(
                        core::concurrent::TaskScheduler::Clock::now());

                scheduler_tick = std::make_shared<std::function<void()>>();
                std::weak_ptr<std::function<void()>> scheduler_tick_weak = scheduler_tick;
                *scheduler_tick = [scheduler_timer, scheduler_tick_weak, scheduler_ptr = &scheduler, scheduler_last_poll_at]() {
                    const auto now = core::concurrent::TaskScheduler::Clock::now();
                    const auto gap = now - *scheduler_last_poll_at;
                    if (gap >= std::chrono::milliseconds(250)) {
                        core::runtime_metrics::record_watchdog_freeze_suspect(
                            "scheduler-poll-loop",
                            std::chrono::duration_cast<std::chrono::nanoseconds>(gap),
                            std::chrono::milliseconds(250),
                            "scheduler-loop-stalled");
                    }
                    *scheduler_last_poll_at = now;
                    scheduler_ptr->poll(32);
                    scheduler_timer->expires_after(std::chrono::milliseconds(50));
                    scheduler_timer->async_wait([scheduler_timer, scheduler_tick_weak, scheduler_ptr, scheduler_last_poll_at](const boost::system::error_code& ec) {
                        if (ec == asio::error::operation_aborted) return;
                        if (!ec) {
                            const auto now = core::concurrent::TaskScheduler::Clock::now();
                            const auto gap = now - *scheduler_last_poll_at;
                            if (gap >= std::chrono::milliseconds(250)) {
                                core::runtime_metrics::record_watchdog_freeze_suspect(
                                    "scheduler-poll-loop",
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(gap),
                                    std::chrono::milliseconds(250),
                                    "scheduler-loop-stalled");
                            }
                            *scheduler_last_poll_at = now;
                            scheduler_ptr->poll(32);
                            if (auto locked = scheduler_tick_weak.lock()) (*locked)();
                        }
                    });
                };
                (*scheduler_tick)();
            },
            [&, bootstrap_task_group]() {
                try {
                    if (scheduler_timer) scheduler_timer->cancel();
                } catch (...) {
                    core::runtime_metrics::record_exception_ignored();
                }
                scheduler_tick.reset();
                scheduler_last_poll_at.reset();
                (void)scheduler.cancel(bootstrap_task_group);
                scheduler.shutdown();
            },
            [&scheduler_tick, &scheduler_timer]() {
                server::core::app::EngineRuntime::WatchdogStatus status;
                status.healthy = static_cast<bool>(scheduler_timer) && static_cast<bool>(scheduler_tick);
                status.detail = status.healthy ? "scheduler-loop-armed" : "scheduler-loop-missing";
                return status;
            });
        engine.start_modules();

        const auto scheduler_runtime_active =
            [&app_host](const core::concurrent::TaskScheduler::RepeatContext&) {
                return !app_host.stop_requested();
            };
        {
            core::concurrent::TaskScheduler::RepeatPolicy policy{};
            policy.interval = std::chrono::seconds(1);
            (void)scheduler.schedule_every_controlled(
                [&engine](const core::concurrent::TaskScheduler::RepeatContext&) {
                    const auto runtime_snapshot = engine.snapshot();
                    auto liveness_state = core::runtime_metrics::LivenessState::kBootstrapping;
                    if (runtime_snapshot.lifecycle_phase == server::core::app::EngineRuntime::LifecyclePhase::kFailed) {
                        liveness_state = core::runtime_metrics::LivenessState::kFailed;
                    } else if (runtime_snapshot.stop_requested) {
                        liveness_state = core::runtime_metrics::LivenessState::kStopping;
                    } else if (!runtime_snapshot.healthy || runtime_snapshot.unhealthy_watchdog_count > 0) {
                        liveness_state = core::runtime_metrics::LivenessState::kDegraded;
                    } else if (runtime_snapshot.ready) {
                        liveness_state = core::runtime_metrics::LivenessState::kRunning;
                    }
                    core::runtime_metrics::set_liveness_state(liveness_state);

                    for (const auto& module : engine.module_snapshot()) {
                        const bool healthy = !module.has_watchdog || module.watchdog_healthy;
                        core::runtime_metrics::record_watchdog_sample(
                            module.name,
                            healthy,
                            module.watchdog_detail);
                    }
                    return core::concurrent::TaskScheduler::RepeatDecision::kContinue;
                },
                policy,
                scheduler_runtime_active,
                bootstrap_task_group);
        }

        if (lua_runtime
            && (!config.lua_scripts_dir.empty() || !config.lua_fallback_scripts_dir.empty())) {
            const std::filesystem::path primary_scripts_dir =
                config.lua_scripts_dir.empty()
                    ? std::filesystem::path{}
                    : std::filesystem::path(config.lua_scripts_dir);
            const std::filesystem::path fallback_scripts_dir =
                config.lua_fallback_scripts_dir.empty()
                    ? std::filesystem::path{}
                    : std::filesystem::path(config.lua_fallback_scripts_dir);

            const auto resolve_effective_scripts_dir =
                [primary_scripts_dir, fallback_scripts_dir](bool& using_fallback) {
                    using_fallback = false;

                    if (!primary_scripts_dir.empty()) {
                        const auto primary_scripts = list_lua_scripts(primary_scripts_dir);
                        if (!primary_scripts.empty()) {
                            return primary_scripts_dir;
                        }
                    }

                    if (!fallback_scripts_dir.empty()) {
                        const auto fallback_scripts = list_lua_scripts(fallback_scripts_dir);
                        if (!fallback_scripts.empty()) {
                            using_fallback = true;
                            return fallback_scripts_dir;
                        }
                    }

                    if (!primary_scripts_dir.empty()) {
                        return primary_scripts_dir;
                    }
                    if (!fallback_scripts_dir.empty()) {
                        using_fallback = true;
                        return fallback_scripts_dir;
                    }

                    return std::filesystem::path{};
                };

            auto active_scripts_dir = std::make_shared<std::filesystem::path>();

            const auto ensure_lua_watcher =
                [resolve_effective_scripts_dir,
                 primary_scripts_dir,
                 fallback_scripts_dir,
                 &config,
                 &lua_script_watcher,
                 active_scripts_dir]() -> bool {
                    bool using_fallback = false;
                    const std::filesystem::path resolved_scripts_dir =
                        resolve_effective_scripts_dir(using_fallback);

                    if (resolved_scripts_dir.empty()) {
                        corelog::warn("Lua runtime enabled but no effective scripts directory resolved");
                        return false;
                    }

                    const bool first_resolution = active_scripts_dir->empty();
                    const bool scripts_dir_changed = (*active_scripts_dir != resolved_scripts_dir);
                    if (!first_resolution && !scripts_dir_changed && lua_script_watcher) {
                        return true;
                    }

                    core::scripting::ScriptWatcher::Config watcher_cfg{};
                    watcher_cfg.scripts_dir = resolved_scripts_dir;
                    watcher_cfg.extensions = {".lua"};
                    watcher_cfg.recursive = true;
                    if (!config.lua_lock_path.empty()) {
                        watcher_cfg.lock_path = std::filesystem::path(config.lua_lock_path);
                    }

                    lua_script_watcher = std::make_shared<core::scripting::ScriptWatcher>(std::move(watcher_cfg));
                    *active_scripts_dir = resolved_scripts_dir;

                    if (using_fallback && !primary_scripts_dir.empty() && !fallback_scripts_dir.empty()) {
                        corelog::info(
                            "Lua scripts fallback selected scripts_dir=" + resolved_scripts_dir.string()
                            + " primary_dir=" + primary_scripts_dir.string());
                    } else {
                        const std::string switch_reason = first_resolution
                            ? "initial"
                            : "switch";
                        corelog::info(
                            "Lua scripts source selected scripts_dir=" + resolved_scripts_dir.string()
                            + " reason=" + switch_reason);
                    }

                    (void)lua_script_watcher->poll(core::scripting::ScriptWatcher::ChangeCallback{});
                    return true;
                };

            if (!ensure_lua_watcher()) {
                corelog::warn("Lua runtime enabled but script watcher could not start");
            } else {
                const auto reload_all_lua_scripts = std::make_shared<std::function<void()>>();
                *reload_all_lua_scripts = [lua_runtime, active_scripts_dir]() {
                    const auto scripts = list_lua_scripts(*active_scripts_dir);
                    std::vector<core::scripting::LuaRuntime::ScriptEntry> entries;
                    entries.reserve(scripts.size());
                    for (const auto& script_path : scripts) {
                        entries.push_back(core::scripting::LuaRuntime::ScriptEntry{
                            script_path,
                            make_lua_env_name(*active_scripts_dir, script_path)
                        });
                    }

                    const auto reload_result = lua_runtime->reload_scripts(entries);
                    if (!reload_result.error.empty()) {
                        corelog::warn(
                            "Lua script reload failed scripts_dir=" + active_scripts_dir->string()
                            + " reason=" + reload_result.error);
                        return;
                    }

                    corelog::info(
                        "Lua script reload complete scripts_dir=" + active_scripts_dir->string()
                        + " loaded=" + std::to_string(reload_result.loaded)
                        + " failed=" + std::to_string(reload_result.failed));
                };

                const auto schedule_lua_reload = [lua_reload_strand, reload_all_lua_scripts]() {
                    if (!lua_reload_strand) {
                        (*reload_all_lua_scripts)();
                        return;
                    }

                    asio::post(*lua_reload_strand, [reload_all_lua_scripts]() {
                        (*reload_all_lua_scripts)();
                    });
                };

                const auto run_lua_reload_sync = [lua_reload_strand, reload_all_lua_scripts, &io]() {
                    if (!lua_reload_strand) {
                        (*reload_all_lua_scripts)();
                        return;
                    }

                    auto promise = std::make_shared<std::promise<void>>();
                    auto future = promise->get_future();
                    asio::post(*lua_reload_strand, [reload_all_lua_scripts, promise]() {
                        (*reload_all_lua_scripts)();
                        promise->set_value();
                    });

                    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                        if (io.poll_one() == 0) {
                            std::this_thread::yield();
                        }
                    }
                    future.get();
                };

                run_lua_reload_sync();

                const auto poll_lua_scripts = [&lua_script_watcher,
                                               ensure_lua_watcher,
                                               schedule_lua_reload,
                                               active_scripts_dir]() {
                    if (!ensure_lua_watcher() || !lua_script_watcher) {
                        return;
                    }

                    std::size_t change_count = 0;
                    const bool poll_ok = lua_script_watcher->poll([&change_count](const core::scripting::ScriptWatcher::ChangeEvent&) {
                        ++change_count;
                    });

                    if (!poll_ok) {
                        corelog::warn("Lua script watcher poll failed scripts_dir=" + active_scripts_dir->string());
                        return;
                    }

                    if (change_count == 0) {
                        return;
                    }

                    corelog::info(
                        "Lua script watcher detected changes scripts_dir=" + active_scripts_dir->string()
                        + " count=" + std::to_string(change_count));
                    schedule_lua_reload();
                };

                poll_lua_scripts();

                const std::uint64_t max_interval = static_cast<std::uint64_t>(std::numeric_limits<long long>::max());
                const std::uint64_t capped_interval =
                    config.lua_reload_interval_ms > max_interval ? max_interval : config.lua_reload_interval_ms;

                core::concurrent::TaskScheduler::RepeatPolicy policy{};
                policy.interval = std::chrono::milliseconds{static_cast<long long>(capped_interval)};
                (void)scheduler.schedule_every_controlled(
                    [poll_lua_scripts](const core::concurrent::TaskScheduler::RepeatContext&) {
                        poll_lua_scripts();
                        return core::concurrent::TaskScheduler::RepeatDecision::kContinue;
                    },
                    policy,
                    scheduler_runtime_active,
                    bootstrap_task_group);
                corelog::info(
                    "Lua script watcher started scripts_dir=" + active_scripts_dir->string()
                    + " interval_ms=" + std::to_string(config.lua_reload_interval_ms));
            }
        }

        // 4. DB 커넥션 풀 구성
        // DB는 server_app에서 사실상 필수 의존성이므로, 이 단계가 애매하게 실패한 채 진행되면 ready 의미가 무너진다.
        std::shared_ptr<core::storage_execution::IConnectionPool> db_pool;
        std::shared_ptr<server::storage::IRepositoryConnectionPool> repository_pool;
        if (!config.db_uri.empty()) {
            corelog::info("Detected DB_URI (redacted)");
            repository_pool = core_internal::make_repository_connection_pool(
                config.db_uri,
                config.db_pool_min,
                config.db_pool_max,
                config.db_conn_timeout_ms,
                config.db_query_timeout_ms,
                config.db_prepare_statements);
            db_pool = repository_pool;
            if (!core_internal::connection_pool_health_check(db_pool)) {
                corelog::error("DB health check failed; please verify DB_URI.");
                core::runtime_metrics::set_liveness_state(core::runtime_metrics::LivenessState::kFailed);
                engine.mark_failed();
                return 2;
            }
            corelog::info("DB connection pool initialised.");
            engine.set_dependency_ok("db", true);
        } else {
            corelog::warn("DB_URI is not set; database features remain disabled.");
            engine.set_dependency_ok("db", false);
        }

        if (db_pool) {
            core_internal::register_connection_pool_service(db_pool);
            if (repository_pool) {
                core_internal::register_repository_connection_pool_service(repository_pool);
            }
            std::size_t log_workers = config.db_worker_threads;
            if (log_workers == 0) {
                log_workers = std::thread::hardware_concurrency();
                if (log_workers == 0) log_workers = 1;
            }
            db_workers = core_internal::make_db_worker_pool(db_pool, config.db_job_queue_max);
            core_internal::start_db_worker_pool(db_workers, config.db_worker_threads);
            core_internal::register_db_worker_pool_service(db_workers);
            corelog::info(std::string("DB worker pool started: ") + std::to_string(log_workers) + " threads.");

            // 주기적인 DB 헬스 체크
            core::concurrent::TaskScheduler::RepeatPolicy policy{};
            policy.interval = std::chrono::seconds(60);
            (void)scheduler.schedule_every_controlled([job_queue_ptr, db_pool, &app_host](
                                                          const core::concurrent::TaskScheduler::RepeatContext&) {
                job_queue_ptr->Push([db_pool, &app_host]() {
                    const bool ok = core_internal::connection_pool_health_check(db_pool);
                    if (!ok) {
                        corelog::warn("Periodic DB health check failed");
                    }
                    app_host.set_dependency_ok("db", ok);
                });
                return core::concurrent::TaskScheduler::RepeatDecision::kContinue;
            },
                                                      policy,
                                                      scheduler_runtime_active,
                                                      bootstrap_task_group);
        }

        // 5. Redis 클라이언트 구성
        // Redis는 배포 토폴로지에 따라 선택 의존성일 수 있지만, 켠 경우에는 fanout/continuity/readiness 의미에 직접 영향을 준다.
        std::shared_ptr<server::core::storage::redis::IRedisClient> redis;
        if (!config.redis_uri.empty()) {
            corelog::info("Detected REDIS_URI (redacted)");
            server::core::storage::redis::Options ropts{};
            ropts.pool_max = config.redis_pool_max;
            ropts.use_streams = config.redis_use_streams;
            redis = core_internal::make_redis_client(config.redis_uri.c_str(), ropts);

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
            engine.set_dependency_ok("redis", ok);
        } else {
            corelog::warn("REDIS_URI is not set; Redis features remain disabled.");
        }

        if (redis) {
            engine.bridge_service(redis);

            const auto load_runtime_assignment =
                [redis, assignment_key = config.topology_runtime_assignment_key, instance_id = config.server_instance_id]()
                -> std::optional<server::core::worlds::TopologyActuationRuntimeAssignmentItem> {
                    if (!redis || assignment_key.empty() || instance_id.empty()) {
                        return std::nullopt;
                    }

                    try {
                        const auto payload = redis->get(assignment_key);
                        if (!payload.has_value() || payload->empty()) {
                            return std::nullopt;
                        }

                        const auto document =
                            server::app::parse_topology_actuation_runtime_assignment_document(*payload);
                        if (!document.has_value()) {
                            return std::nullopt;
                        }

                        return server::app::find_topology_actuation_runtime_assignment_for_instance(
                            *document,
                            instance_id);
                    } catch (...) {
                        return std::nullopt;
                    }
                };
            const auto apply_runtime_assignment =
                [base_shard = config.server_shard, base_tags = config.server_tags, load_runtime_assignment](
                    server::core::state::InstanceRecord& record) {
                    const auto assignment = load_runtime_assignment();
                    record.shard = server::app::resolve_topology_runtime_assignment_shard(base_shard, assignment);
                    record.tags = server::app::apply_topology_runtime_assignment_tags(base_tags, assignment);
                };

            // 주기적인 Redis 헬스 체크
            core::concurrent::TaskScheduler::RepeatPolicy policy{};
            policy.interval = std::chrono::seconds(60);
            (void)scheduler.schedule_every_controlled([job_queue_ptr, redis, &app_host](
                                                          const core::concurrent::TaskScheduler::RepeatContext&) {
                job_queue_ptr->Push([redis, &app_host]() {
                    try {
                        const bool ok = redis->health_check();
                        if (!ok) {
                            corelog::warn("Periodic Redis health check failed");
                        }
                        app_host.set_dependency_ok("redis", ok);
                    } catch (const std::exception& ex) {
                        core::runtime_metrics::record_exception_recoverable();
                        corelog::error(std::string("component=server_bootstrap error_code=REDIS_HEALTH_CHECK periodic Redis health check exception: ") + ex.what());
                    } catch (...) {
                        core::runtime_metrics::record_exception_ignored();
                        corelog::error("component=server_bootstrap error_code=REDIS_HEALTH_CHECK periodic Redis health check unknown exception");
                    }
                });
                return core::concurrent::TaskScheduler::RepeatDecision::kContinue;
            },
                                                      policy,
                                                      scheduler_runtime_active,
                                                      bootstrap_task_group);

            // 인스턴스 레지스트리에 서버 등록
            try {
                registry_backend = core_internal::make_registry_backend(
                    redis, config.registry_prefix, config.registry_ttl);

                registry_record.instance_id = config.server_instance_id;
                registry_record.host = config.advertise_host;
                registry_record.port = config.advertise_port;
                registry_record.role = config.server_role;
                registry_record.game_mode = config.server_game_mode;
                registry_record.region = config.server_region;
                registry_record.shard = config.server_shard;
                registry_record.tags = config.server_tags;
                registry_record.capacity = 0;
                registry_record.active_sessions = 0;
                registry_record.ready = app_host.ready();
                registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                apply_runtime_assignment(registry_record);

                if (registry_backend->upsert(registry_record)) {
                    registry_registered = true;
                    corelog::info("Registered server instance id=" + registry_record.instance_id +
                                  " host=" + registry_record.host + ":" + std::to_string(registry_record.port));
                } else {
                    corelog::warn("Failed to register server instance in registry");
                }
            } catch (const std::exception& ex) {
                core::runtime_metrics::record_exception_recoverable();
                corelog::warn(std::string("component=server_bootstrap error_code=REGISTRY_INIT_FAILED failed to initialise server registry backend: ") + ex.what());
            }

            // 레지스트리 하트비트 스케줄링
            if (registry_registered) {
                core::concurrent::TaskScheduler::RepeatPolicy policy{};
                policy.interval = config.registry_heartbeat_interval;
                (void)scheduler.schedule_every_controlled(
                    [registry_backend, registry_record, state, &app_host, apply_runtime_assignment](
                        const core::concurrent::TaskScheduler::RepeatContext&) mutable {
                    registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                    registry_record.active_sessions = core_internal::connection_count(state);
                    registry_record.ready = app_host.ready();
                    apply_runtime_assignment(registry_record);
                    if (!registry_backend->upsert(registry_record)) {
                        corelog::warn("Server registry heartbeat upsert failed");
                    }
                    return core::concurrent::TaskScheduler::RepeatDecision::kContinue;
                },
                    policy,
                    scheduler_runtime_active,
                    bootstrap_task_group);
            }
        }

        // 6. 채팅 서비스 및 라우터 초기화
        // 외부 의존성이 올라온 뒤에 서비스와 dispatcher를 묶어야, 핸들러가 시작부터 완성된 runtime seam을 본다.
        server::app::chat::ChatService chat(io, job_queue, repository_pool, redis);
        server::app::fps::FpsService fps(server::app::fps::RuntimeConfig{
            .tick_rate_hz = config.fps_tick_rate_hz,
            .snapshot_refresh_ticks = config.fps_snapshot_refresh_ticks,
            .interest_cell_size_mm = config.fps_interest_cell_size_mm,
            .interest_radius_cells = config.fps_interest_radius_cells,
            .max_interest_recipients_per_tick = config.fps_max_interest_recipients_per_tick,
            .max_delta_actors_per_tick = config.fps_max_delta_actors_per_tick,
            .history_ticks = config.fps_history_ticks,
        });
        engine.bridge_alias(chat);
        engine.bridge_alias(fps);
        register_routes(dispatcher, chat, fps);

        fps_tick_timer = std::make_shared<asio::steady_timer>(io);
        auto fps_driver = std::make_shared<server::app::fps::FixedStepDriver>(config.fps_tick_rate_hz);
        auto fps_last_tick = std::make_shared<server::app::fps::FixedStepDriver::Clock::time_point>(
            server::app::fps::FixedStepDriver::Clock::now());
        auto fps_tick = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> fps_tick_weak = fps_tick;
        *fps_tick = [fps_tick_timer, fps_tick_weak, fps_driver, fps_last_tick, fps_ptr = &fps]() {
            const auto now = server::app::fps::FixedStepDriver::Clock::now();
            const auto elapsed = now - *fps_last_tick;
            *fps_last_tick = now;

            const auto steps = fps_driver->consume_elapsed(elapsed);
            for (std::size_t step = 0; step < steps; ++step) {
                fps_ptr->tick();
            }

            fps_tick_timer->expires_after(fps_driver->step_duration());
            fps_tick_timer->async_wait([fps_tick_timer, fps_tick_weak](const boost::system::error_code& ec) {
                if (ec == asio::error::operation_aborted) {
                    return;
                }
                if (!ec) {
                    if (auto locked = fps_tick_weak.lock()) {
                        (*locked)();
                    }
                }
            });
        };
        (*fps_tick)();

        // 7. TCP 리스너 시작
        // 이 단계 이전에는 절대로 외부 연결을 받지 않는다. 포트를 너무 일찍 열면 ready=false인데도 클라이언트가 붙을 수 있다.
        auto acceptor = core_internal::make_session_listener_handle(
            io,
            config.port,
            dispatcher,
            buffer_manager,
            options,
            state,
            [&chat, &fps](std::shared_ptr<server::core::Session> session) {
                fps.on_session_close(session);
                chat.on_session_close(session);
            });
        engine.bridge_service(acceptor);
        acceptor->start();
        corelog::info("server_app listening on 0.0.0.0:" + std::to_string(config.port));

        // 8. 워커 스레드 풀 시작
        unsigned int num_worker_threads = std::max(1u, std::thread::hardware_concurrency());
        workers.Start(num_worker_threads);
        corelog::info(std::to_string(num_worker_threads) + " worker threads started.");

        // 9. I/O 스레드 풀 시작
        // accept보다 실행기를 늦게 올리면 큐에 쌓인 작업이 소비되지 않는 구간이 생기므로, ready를 열기 직전에 둘 다 보장한다.
        unsigned int num_io_threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> io_threads;
        io_threads.reserve(num_io_threads);
        for (unsigned int i = 0; i < num_io_threads; ++i) {
            io_threads.emplace_back([&io]() { 
                try { io.run(); } catch (const std::exception& e) {
                    core::runtime_metrics::record_exception_recoverable();
                    corelog::error(std::string("component=server_bootstrap error_code=IO_THREAD_EXCEPTION I/O thread exception: ") + e.what());
                }
            });
        }
        corelog::info(std::to_string(num_io_threads) + " I/O threads started.");

        // ready는 기본 ready 플래그와 의존성 probe를 함께 본다.
        // 둘 중 하나만 보면 "리스너는 켜졌지만 외부 저장소는 아직 죽어 있는" 상태를 정상으로 잘못 노출할 수 있다.
        engine.mark_running();
        core::runtime_metrics::set_liveness_state(core::runtime_metrics::LivenessState::kRunning);

        if (registry_registered && registry_backend) {
            registry_record.last_heartbeat_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            registry_record.active_sessions = core_internal::connection_count(state);
            registry_record.ready = app_host.ready();
            registry_record.shard = config.server_shard;
            registry_record.tags = config.server_tags;
            if (redis && !config.topology_runtime_assignment_key.empty()) {
                try {
                    const auto payload = redis->get(config.topology_runtime_assignment_key);
                    if (payload.has_value() && !payload->empty()) {
                        const auto document =
                            server::app::parse_topology_actuation_runtime_assignment_document(*payload);
                        if (document.has_value()) {
                            const auto assignment =
                                server::app::find_topology_actuation_runtime_assignment_for_instance(
                                    *document,
                                    config.server_instance_id);
                            registry_record.shard = server::app::resolve_topology_runtime_assignment_shard(
                                config.server_shard,
                                assignment);
                            registry_record.tags = server::app::apply_topology_runtime_assignment_tags(
                                config.server_tags,
                                assignment);
                        }
                    }
                } catch (...) {
                }
            }
            if (!registry_backend->upsert(registry_record)) {
                corelog::warn("Server registry ready-state upsert failed");
            }
        }

        auto admin_command_verifier = make_admin_command_verifier(config);
        const auto local_instance_selector_context = make_local_instance_selector_context(config);

        // 10. Redis Pub/Sub 구독 (분산 채팅용)
        start_chat_fanout_subscription(
            chat,
            redis,
            config,
            admin_command_verifier,
            local_instance_selector_context);

        // 11. metrics/admin HTTP 시작
        // 운영 면은 데이터 plane과 거의 같이 열어 두되, ready 판정은 AppHost가 따로 쥔다.
        // 그래야 관측은 일찍 가능하면서도 실제 트래픽 수용 시점은 엄격하게 제어할 수 있다.
        start_server_admin_http(engine, config.metrics_port);

        // 12. 종료 시그널 대기 및 shutdown 순서 등록
        // drain -> accept 중지 -> 워커/타이머 정리 순서를 명시해 새 연결 유입과 기존 연결 정리를 섞지 않는다.
        register_server_shutdown_steps(
            engine,
            ServerShutdownContext{
                .workers = &workers,
                .io = &io,
                .connection_state = state,
                .acceptor = acceptor,
                .db_workers = db_workers,
                .lua_runtime = lua_runtime,
                .redis = redis,
                .registry_registered = &registry_registered,
                .registry_backend = registry_backend,
                .registry_record = &registry_record,
                .shutdown_drain_timeout_ms = config.shutdown_drain_timeout_ms,
                .shutdown_drain_poll_ms = config.shutdown_drain_poll_ms,
            });

        engine.install_asio_termination_signals(io, {});

        for (auto& t : io_threads) {
            t.join();
        }

        core::runtime_metrics::set_liveness_state(core::runtime_metrics::LivenessState::kStopping);
        engine.run_shutdown();
        engine.mark_stopped();

        engine.clear_global_services();

        return 0;

    } catch (const std::exception& ex) {
        core::runtime_metrics::record_exception_fatal();
        core::runtime_metrics::set_liveness_state(core::runtime_metrics::LivenessState::kFailed);
        corelog::error(std::string("component=server_bootstrap error_code=SERVER_FATAL server_app exception: ") + ex.what());
        engine.mark_failed();
        engine.clear_global_services();
        return 1;
    }
}

} // namespace server::app
