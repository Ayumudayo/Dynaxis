// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <optional>
#include <sstream>
#include <atomic>
#include <cstdint>
#include <limits>
#include <random>

#include "../wb_common/redis_client_factory.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/app/engine_builder.hpp"
#include "server/core/storage_execution/retry_backoff.hpp"
#include "server/core/util/log.hpp"
#include "server/core/trace/context.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/metrics.hpp"
#include <pqxx/pqxx>
#include <nlohmann/json.hpp>

using server::core::log::info;
using json = nlohmann::json;

namespace {

// -----------------------------------------------------------------------------
// 설정 (Configuration)
// -----------------------------------------------------------------------------
struct WorkerConfig {
    std::string db_uri;
    std::string redis_uri;
    std::string stream_key = "session_events";
    std::string group = "wb_group";
    std::string consumer = "wb_consumer";
    std::string dlq_stream = "session_events_dlq";
    
    std::size_t batch_max_events = 100;
    std::size_t batch_max_bytes = 512 * 1024;
    long long batch_delay_ms = 500;
    
    bool dlq_on_error = true;
    bool ack_on_error = true;

    // 미처리 재회수(PEL reclaim) 설정
    // 워커가 죽거나 처리 중 멈췄던 메시지를 언제 다시 가져올지 정하는 안전장치다.
    bool reclaim_enabled = true;
    long long reclaim_interval_ms = 1000;
    long long reclaim_min_idle_ms = 5000;
    std::size_t reclaim_count = 200;

    std::uint16_t metrics_port = 0;

    long long db_reconnect_base_ms = 500;
    long long db_reconnect_max_ms = 30000;
    std::uint32_t retry_max = 5;
    long long retry_backoff_ms = 250;

    static WorkerConfig Load() {
        WorkerConfig cfg;
        // 설정은 현재 프로세스 환경 변수에서만 읽는다.
        // 여기서 별도 `.env`를 다시 해석하지 않는 이유는 배포 환경이 이미 결정한 값을 도구가 임의로 덮어쓰지 않게 하기 위해서다.

        if (const char* v = std::getenv("DB_URI")) cfg.db_uri = v;
        if (const char* v = std::getenv("REDIS_URI")) cfg.redis_uri = v;
        if (const char* v = std::getenv("REDIS_STREAM_KEY")) cfg.stream_key = v;
        if (const char* v = std::getenv("WB_GROUP")) cfg.group = v;
        if (const char* v = std::getenv("WB_CONSUMER")) cfg.consumer = v;
        if (const char* v = std::getenv("WB_DLQ_STREAM")) cfg.dlq_stream = v;

        if (const char* v = std::getenv("METRICS_PORT")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n > 0 && n <= 65535) cfg.metrics_port = static_cast<std::uint16_t>(n);
        }

        if (const char* v = std::getenv("WB_DB_RECONNECT_BASE_MS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n >= 100 && n <= 60000) cfg.db_reconnect_base_ms = static_cast<long long>(n);
        }
        if (const char* v = std::getenv("WB_DB_RECONNECT_MAX_MS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n >= 1000 && n <= 300000) cfg.db_reconnect_max_ms = static_cast<long long>(n);
        }
        if (cfg.db_reconnect_max_ms < cfg.db_reconnect_base_ms) {
            cfg.db_reconnect_max_ms = cfg.db_reconnect_base_ms;
        }

        if (const char* v = std::getenv("WB_RETRY_MAX")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n <= 100) cfg.retry_max = static_cast<std::uint32_t>(n);
        }
        if (const char* v = std::getenv("WB_RETRY_BACKOFF_MS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n >= 10 && n <= 10000) cfg.retry_backoff_ms = static_cast<long long>(n);
        }

        if (const char* v = std::getenv("WB_BATCH_MAX_EVENTS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n > 0 && n <= 10000) cfg.batch_max_events = static_cast<std::size_t>(n);
        }
        if (const char* v = std::getenv("WB_BATCH_MAX_BYTES")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n >= 16 * 1024 && n <= 16 * 1024 * 1024) cfg.batch_max_bytes = static_cast<std::size_t>(n);
        }
        if (const char* v = std::getenv("WB_BATCH_DELAY_MS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n >= 50 && n <= 10000) cfg.batch_delay_ms = static_cast<long long>(n);
        }
        
        if (const char* v = std::getenv("WB_DLQ_ON_ERROR")) cfg.dlq_on_error = (std::string(v) != "0");
        if (const char* v = std::getenv("WB_ACK_ON_ERROR")) cfg.ack_on_error = (std::string(v) != "0");

        if (const char* v = std::getenv("WB_RECLAIM_ENABLED")) cfg.reclaim_enabled = (std::string(v) != "0");
        if (const char* v = std::getenv("WB_RECLAIM_INTERVAL_MS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n >= 100 && n <= 60 * 1000) cfg.reclaim_interval_ms = static_cast<long long>(n);
        }
        if (const char* v = std::getenv("WB_RECLAIM_MIN_IDLE_MS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n <= 10 * 60 * 1000) cfg.reclaim_min_idle_ms = static_cast<long long>(n);
        }
        if (const char* v = std::getenv("WB_RECLAIM_COUNT")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n > 0 && n <= 10000) cfg.reclaim_count = static_cast<std::size_t>(n);
        }

        return cfg;
    }
};

// -----------------------------------------------------------------------------
// 유틸리티 (Utilities)
// -----------------------------------------------------------------------------
bool IsUuid(const std::string& s) {
    if (s.size() != 36) return false;
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    const int dashes[4] = {8, 13, 18, 23};
    for (int i = 0, j = 0; i < 36; ++i) {
        if (j < 4 && i == dashes[j]) {
            if (s[i] != '-') return false;
            ++j;
        } else if (!is_hex(s[i])) {
            return false;
        }
    }
    return true;
}

std::size_t EstimateEntryBytes(const server::core::storage::redis::IRedisClient::StreamEntry& e) {
    std::size_t est = e.id.size();
    for (const auto& f : e.fields) {
        est += f.first.size() + f.second.size() + 4;
    }
    return est;
}

} // namespace

// -----------------------------------------------------------------------------
// Write-Behind 워커
// -----------------------------------------------------------------------------
/**
 * @brief write-behind 패턴을 실행하는 독립 워커입니다.
 *
 * 이 프로세스는 메인 서버가 직접 DB 쓰기 지연을 감당하지 않도록, Redis Stream에 적재된 이벤트를 뒤에서 읽어
 * PostgreSQL에 반영합니다. 목적은 단순한 비동기화가 아니라 "실시간 응답 경로"와 "느린 영속화 경로"를 분리해
 * 서로의 장애를 바로 전염시키지 않게 하는 것입니다.
 *
 * 왜 별도 워커인가:
 * - 서버 요청 경로에서 DB를 바로 기다리면 꼬리 지연시간(tail latency)이 급격히 늘고, 트래픽 급증 시 DB가 병목이 된다.
 * - 반대로 영속화를 완전히 버리면 감사, 재처리, 운영 분석에 필요한 사실이 사라진다.
 * - 그래서 스트림을 사이에 둬 적어도 한 번(at-least-once) 적재 경로를 따로 두고, 실패는 DLQ/재시도로 분리해 다룬다.
 *
 * 주요 흐름:
 * 1. Redis Stream(`session_events`)에서 consumer group으로 이벤트를 읽는다.
 * 2. 이벤트를 내부 버퍼에 모아 배치 단위로 DB에 반영한다.
 * 3. commit 성공 후에만 ACK 하여 적어도 한 번은 처리되게 유지한다.
 * 4. 영구적 오류는 DLQ로 옮겨 무한 재시도를 피하고, 일시 장애는 재연결/재시도로 흡수한다.
 */
class WbWorker {
public:
    explicit WbWorker(WorkerConfig config)
        : engine_(server::core::app::EngineBuilder("wb_worker")
                      .install_process_signal_handlers()
                      .build())
        , app_host_(engine_.host())
        , config_(std::move(config)) {}

    int Run() {
        // 이 워커의 ready는 Redis와 DB가 둘 다 살아 있어야만 true가 된다.
        // 둘 중 하나라도 빠지면 "이벤트를 읽기만 하거나 쓰기만 하는 반쪽 상태"가 되므로 정상 처리라고 볼 수 없다.
        engine_.declare_dependency("redis");
        engine_.declare_dependency("db");
        engine_.set_dependency_ok("redis", false);
        engine_.set_dependency_ok("db", false);
        engine_.set_ready(false);

        if (config_.db_uri.empty()) {
            std::cerr << "WB worker: DB_URI not set" << std::endl;
            engine_.mark_failed();
            return 2;
        }
        if (config_.redis_uri.empty()) {
            std::cerr << "WB worker: REDIS_URI not set" << std::endl;
            engine_.mark_failed();
            return 2;
        }

        if (config_.ack_on_error && !config_.dlq_on_error) {
            server::core::log::warn(
                "WB worker configured with WB_ACK_ON_ERROR=1 and WB_DLQ_ON_ERROR=0; failed events may be dropped"
            );
        }

        // Redis는 입력 큐 자체이므로 시작 시점에 반드시 붙어 있어야 한다.
        server::core::storage::redis::Options ropts{};
        redis_ = wb_tools::make_redis_client(config_.redis_uri, ropts);
        if (!redis_ || !redis_->health_check()) {
            std::cerr << "WB worker: Redis health check failed" << std::endl;
            engine_.mark_failed();
            return 3;
        }
        NoteRedisAvailability(true, "startup_health_check");
        engine_.set_service(redis_);

        // consumer group은 여러 워커가 같은 스트림을 나눠 처리할 때 중복 소비를 줄이는 기본 경계다.
        // 이미 존재하는 경우를 성공으로 보는 이유는, 재기동이나 scale-out이 매번 새 그룹을 만들면 안 되기 때문이다.
        (void)redis_->xgroup_create_mkstream(config_.stream_key, config_.group);
        info("WB worker consuming stream=" + config_.stream_key + 
             ", group=" + config_.group + ", consumer=" + config_.consumer);

        // DB는 시작 시점에 붙는 것이 이상적이지만, 초기 실패만으로 즉시 종료하지는 않는다.
        // 재연결 루프를 갖춘 프로세스이므로, 일시 DB 장애는 런타임 복구 대상으로 다루는 편이 운영상 더 낫다.
        if (!EnsureDbConnection()) {
            std::cerr << "WB worker: Initial DB connection failed" << std::endl;
        }

        engine_.start_admin_http(config_.metrics_port, [this]() { return RenderMetrics(); });
        engine_.mark_running();

        // 메인 루프는 "DB가 준비되지 않으면 읽지 않는다"보다 "읽은 뒤 확인 응답(ACK)을 섣불리 하지 않는다"를 더 강하게 보장해야 한다.
        Loop();
        engine_.run_shutdown();
        engine_.mark_stopped();
        return 0;
    }

private:
    bool EnsureDbConnection() {
        if (db_ && db_->is_open()) {
            if (EnsureDbPrepared()) {
                app_host_.set_dependency_ok("db", true);
                return true;
            }
            app_host_.set_dependency_ok("db", false);
            wb_db_unavailable_total_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        try {
            db_prepared_ = false;
            db_ = std::make_unique<pqxx::connection>(config_.db_uri);
            if (db_->is_open()) {
                info("DB connected successfully.");
                if (!EnsureDbPrepared()) {
                    server::core::log::warn("DB connected but prepare failed; will retry");
                    app_host_.set_dependency_ok("db", false);
                    return false;
                }
                app_host_.set_dependency_ok("db", true);
                return true;
            }
        } catch (const std::exception& e) {
            std::cerr << "DB connection attempt failed: " << e.what() << std::endl;
        }
        app_host_.set_dependency_ok("db", false);
        wb_db_unavailable_total_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool EnsureDbPrepared() {
        if (!db_ || !db_->is_open()) {
            return false;
        }
        if (db_prepared_) {
            return true;
        }

        static constexpr const char* kInsertName = "wb_insert_session_event";
        static constexpr const char* kInsertSql =
            "insert into session_events(event_id, type, ts, user_id, session_id, room_id, payload) "
            "values ($1, $2, to_timestamp(($3)::bigint/1000.0), "
            "nullif($4,'')::uuid, nullif($5,'')::uuid, nullif($6,'')::uuid, $7::jsonb) "
            "on conflict (event_id) do nothing";

        try {
            db_->prepare(kInsertName, kInsertSql);
        } catch (const pqxx::usage_error&) {
            // 이미 prepare된 이름일 수 있으므로(재사용 등) 이를 성공으로 처리합니다.
        } catch (const pqxx::argument_error&) {
            // 이미 prepare된 이름일 수 있으므로(재사용 등) 이를 성공으로 처리합니다.
        } catch (const pqxx::broken_connection&) {
            db_prepared_ = false;
            return false;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("DB prepare failed: ") + e.what());
            db_prepared_ = false;
            return false;
        }

        db_prepared_ = true;
        return true;
    }

    bool Ack(const std::string& id) {
        if (!redis_) {
            return false;
        }
        const bool ok = redis_->xack(config_.stream_key, config_.group, id);
        if (ok) {
            wb_ack_total_.fetch_add(1, std::memory_order_relaxed);
        } else {
            wb_ack_fail_total_.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }

    void NoteRedisAvailability(bool ok, std::string_view operation) {
        const bool previous = redis_dependency_ok_.exchange(ok, std::memory_order_relaxed);
        app_host_.set_dependency_ok("redis", ok);
        if (ok) {
            if (!previous) {
                server::core::log::info("Redis dependency recovered after op=" + std::string(operation));
            }
            return;
        }

        wb_redis_unavailable_total_.fetch_add(1, std::memory_order_relaxed);
        if (previous) {
            server::core::log::warn("Redis dependency unavailable during op=" + std::string(operation));
        }
    }

    void ResetDbReconnectBackoff() {
        db_reconnect_attempt_ = 0;
        wb_db_reconnect_backoff_ms_last_.store(0, std::memory_order_relaxed);
    }

    void SleepDbReconnectBackoff() {
        const auto delay = server::core::storage_execution::sample_retry_backoff_delay_ms(
            server::core::storage_execution::RetryBackoffPolicy{
                .mode = server::core::storage_execution::RetryBackoffMode::kExponentialFullJitter,
                .base_delay_ms = static_cast<std::uint64_t>(std::max<long long>(1, config_.db_reconnect_base_ms)),
                .max_delay_ms = static_cast<std::uint64_t>(
                    std::max<long long>(std::max<long long>(1, config_.db_reconnect_base_ms), config_.db_reconnect_max_ms)),
            },
            db_reconnect_attempt_,
            rng_);

        wb_db_reconnect_backoff_ms_last_.store(delay, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        if (db_reconnect_attempt_ < std::numeric_limits<std::uint32_t>::max()) {
            ++db_reconnect_attempt_;
        }
    }

    void Loop() {
        auto last_flush = std::chrono::steady_clock::now();
        auto last_pending_log = std::chrono::steady_clock::now();
        auto last_reclaim = std::chrono::steady_clock::now() - std::chrono::milliseconds(config_.reclaim_interval_ms);
        bool initial_reclaim_done = false;
        
        std::vector<server::core::storage::redis::IRedisClient::StreamEntry> buf;
        buf.reserve(config_.batch_max_events);
        std::size_t buf_bytes = 0;

        while (!app_host_.stop_requested()) {
            // DB가 끊겨 있으면 ready를 내리고 재연결 백오프로 들어간다.
            // 이때 스트림 소비를 계속 밀어붙이면 ACK/commit 경계가 흐려져 pending만 쌓이고 복구가 더 어려워진다.
            if (!EnsureDbConnection()) {
                app_host_.set_ready(false);
                SleepDbReconnectBackoff();
                continue;
            }

            ResetDbReconnectBackoff();
            app_host_.set_ready(true);

            // 0. 미처리 재회수(PEL reclaim)
            // 이전 워커가 잡고 죽은 메시지나 오래 걸린 메시지를 다시 가져와 정체를 풀어 주는 단계다.
            {
                const auto now = std::chrono::steady_clock::now();
                if (config_.reclaim_enabled &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reclaim).count() >= config_.reclaim_interval_ms) {
                    const long long min_idle = initial_reclaim_done ? config_.reclaim_min_idle_ms : 0;
                    initial_reclaim_done = true;
                    ReclaimPending(min_idle, buf, buf_bytes, last_flush);
                    last_reclaim = now;
                }
            }

            // 1. Redis에서 메시지 읽기 (blocking)
            // 짧은 block timeout을 두어 빈 루프 바쁜 대기(busy-spin)를 피하면서도 종료 신호와 재연결 판단에 빨리 반응한다.
            std::vector<server::core::storage::redis::IRedisClient::StreamEntry> entries;
            if (!redis_->xreadgroup(config_.stream_key, config_.group, config_.consumer, 
                                  500, config_.batch_max_events, entries)) {
                NoteRedisAvailability(false, "xreadgroup");
                // Redis read 실패는 일시 장애일 수 있으므로 짧게 쉬고 다시 본다.
                // 곧바로 tight loop로 돌면 장애 시 로그 폭주와 CPU 낭비가 커진다.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue; 
            }
            NoteRedisAvailability(true, "xreadgroup");

            // 2. 버퍼링
            // 이벤트를 한 건씩 바로 commit하면 안전해 보여도 처리량이 급격히 떨어지므로, 배치 단위와 지연시간 상한을 같이 둔다.
            if (!entries.empty()) {
                for (auto& e : entries) {
                    buf_bytes += EstimateEntryBytes(e);
                    buf.emplace_back(std::move(e));

                    // 배치 크기나 총 바이트 상한을 넘으면 즉시 flush한다.
                    // 큰 payload가 몰릴 때 건수만 보면 메모리가 예상보다 빨리 불어날 수 있다.
                    if (buf.size() >= config_.batch_max_events || buf_bytes >= config_.batch_max_bytes) {
                        Flush(buf);
                        buf_bytes = 0;
                        last_flush = std::chrono::steady_clock::now();
                    }
                }
            }

            // 3. 시간 기반 플러시
            // 트래픽이 낮을 때도 버퍼가 너무 오래 머물지 않게 해, write-behind가 무기한 지연 큐로 변질되지 않게 한다.
            auto now = std::chrono::steady_clock::now();
            if (!buf.empty() && 
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= config_.batch_delay_ms) {
                Flush(buf);
                buf_bytes = 0;
                last_flush = now;
            }

            // 4. pending 상태 모니터링
            // 적체량(backlog)을 계속 관찰해야 "워커가 느린가", "ACK가 막혔는가", "PEL reclaim이 밀리는가"를 운영에서 구분할 수 있다.
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pending_log).count() >= 1000) {
                long long pending = 0;
                if (redis_->xpending(config_.stream_key, config_.group, pending)) {
                    NoteRedisAvailability(true, "xpending");
                    wb_pending_.store(pending, std::memory_order_relaxed);
                    server::core::log::info("metric=wb_pending value=" + std::to_string(pending));
                } else {
                    NoteRedisAvailability(false, "xpending");
                }
                last_pending_log = now;
            }
        }
    }

    void ReclaimPending(long long min_idle_ms,
                        std::vector<server::core::storage::redis::IRedisClient::StreamEntry>& buf,
                        std::size_t& buf_bytes,
                        std::chrono::steady_clock::time_point& last_flush) {
        if (!redis_) {
            return;
        }

        server::core::storage::redis::IRedisClient::StreamAutoClaimResult claimed;
        wb_reclaim_runs_total_.fetch_add(1, std::memory_order_relaxed);
        if (!redis_->xautoclaim(config_.stream_key,
                                config_.group,
                                config_.consumer,
                                min_idle_ms,
                                reclaim_next_id_,
                                config_.reclaim_count,
                                claimed)) {
            NoteRedisAvailability(false, "xautoclaim");
            wb_reclaim_error_total_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        NoteRedisAvailability(true, "xautoclaim");

        if (!claimed.next_start_id.empty()) {
            reclaim_next_id_ = claimed.next_start_id;
        }

        if (!claimed.deleted_ids.empty()) {
            wb_reclaim_deleted_total_.fetch_add(static_cast<std::uint64_t>(claimed.deleted_ids.size()), std::memory_order_relaxed);
            // Stream에서 이미 삭제된 메시지는 더 이상 처리할 수 없으므로 PEL에서 제거한다.
            for (const auto& id : claimed.deleted_ids) {
                (void)Ack(id);
            }
        }

        if (claimed.entries.empty()) {
            return;
        }

        wb_reclaim_total_.fetch_add(static_cast<std::uint64_t>(claimed.entries.size()), std::memory_order_relaxed);

        for (auto& e : claimed.entries) {
            buf_bytes += EstimateEntryBytes(e);
            buf.emplace_back(std::move(e));
            if (buf.size() >= config_.batch_max_events || buf_bytes >= config_.batch_max_bytes) {
                Flush(buf);
                buf_bytes = 0;
                last_flush = std::chrono::steady_clock::now();
            }
        }
    }

    // 버퍼에 쌓인 이벤트를 DB에 저장하고 Redis에 ACK 처리한다.
    //
    // 핵심 규칙:
    // 1) 배치 단위 트랜잭션으로 처리량을 확보하되, 이벤트 단위 실패는 savepoint로 격리해 전체 배치를 살린다.
    // 2) commit 성공 후에만 ACK 해 적어도 한 번(at-least-once) 적재 의미를 지킨다.
    // 3) 영구적 오류는 DLQ로 분리하고, ack_on_error 정책은 데이터 유실 가능성을 운영자가 명시적으로 선택한 경우에만 따른다.
    void Flush(std::vector<server::core::storage::redis::IRedisClient::StreamEntry>& buf) {
        if (buf.empty()) return;

        auto t0 = std::chrono::steady_clock::now();
        std::size_t ok = 0;
        std::size_t fail = 0;
        std::size_t dlqed = 0;

        bool committed = false;
        for (std::uint32_t attempt = 0; attempt <= config_.retry_max && !committed; ++attempt) {
            if (attempt > 0) {
                wb_flush_retry_attempt_total_.fetch_add(1, std::memory_order_relaxed);

                const auto delay_ms = server::core::storage_execution::retry_backoff_upper_bound_ms(
                    server::core::storage_execution::RetryBackoffPolicy{
                        .mode = server::core::storage_execution::RetryBackoffMode::kLinear,
                        .base_delay_ms = static_cast<std::uint64_t>(std::max<long long>(1, config_.retry_backoff_ms)),
                        .max_delay_ms = 30000ull,
                    },
                    attempt);
                wb_flush_retry_delay_ms_last_.store(delay_ms, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

            std::vector<std::string> ack_ids;
            ack_ids.reserve(buf.size());
            std::size_t attempt_fail = 0;
            std::size_t attempt_dlqed = 0;

            try {
                if (!db_ || !db_->is_open() || !EnsureDbPrepared()) {
                    throw std::runtime_error("DB not ready");
                }

                pqxx::work tx(*db_);

                for (const auto& e : buf) {
                    try {
                        pqxx::subtransaction sub(tx, "wb_entry");
                        ProcessEntry(sub, e);
                        sub.commit();
                        ack_ids.emplace_back(e.id);
                    } catch (const pqxx::broken_connection&) {
                        throw;
                    } catch (const std::exception& ex) {
                        ++attempt_fail;
                        bool acked = false;

                        // 영구적 오류는 DLQ로 이동한다.
                        // 그대로 pending에 남겨 두면 같은 독성 이벤트가 배치를 계속 막아 전체 처리량을 떨어뜨릴 수 있다.
                        if (config_.dlq_on_error && !config_.dlq_stream.empty()) {
                            if (SendToDlq(e, ex.what())) {
                                (void)Ack(e.id);
                                acked = true;
                                ++attempt_dlqed;
                            }
                        }

                        if (!acked && config_.ack_on_error) {
                            if (Ack(e.id)) {
                                wb_error_drop_total_.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }

                tx.commit();
                committed = true;

                ok += ack_ids.size();
                fail += attempt_fail;
                dlqed += attempt_dlqed;

                for (const auto& id : ack_ids) {
                    (void)Ack(id);
                }
            } catch (const pqxx::broken_connection&) {
                fail += attempt_fail;
                dlqed += attempt_dlqed;
                std::cerr << "DB connection broken during flush. retrying..." << std::endl;
                db_.reset();
                db_prepared_ = false;
            } catch (const std::exception& ex) {
                fail += attempt_fail;
                dlqed += attempt_dlqed;
                server::core::log::error(std::string("WB flush DB error: ") + ex.what());
            }

            if (!committed && attempt == config_.retry_max) {
                wb_flush_retry_exhausted_total_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        wb_flush_total_.fetch_add(1, std::memory_order_relaxed);
        wb_flush_ok_total_.fetch_add(static_cast<std::uint64_t>(ok), std::memory_order_relaxed);
        wb_flush_fail_total_.fetch_add(static_cast<std::uint64_t>(fail), std::memory_order_relaxed);
        wb_flush_dlq_total_.fetch_add(static_cast<std::uint64_t>(dlqed), std::memory_order_relaxed);
        wb_flush_batch_size_last_.store(static_cast<std::uint64_t>(buf.size()), std::memory_order_relaxed);

        const auto ms_u = static_cast<std::uint64_t>(ms < 0 ? 0 : ms);
        wb_flush_commit_ms_last_.store(ms_u, std::memory_order_relaxed);
        wb_flush_commit_ms_sum_.fetch_add(ms_u, std::memory_order_relaxed);
        wb_flush_commit_ms_count_.fetch_add(1, std::memory_order_relaxed);
        {
            auto& max_ref = wb_flush_commit_ms_max_;
            auto current_max = max_ref.load(std::memory_order_relaxed);
            while (current_max < ms_u &&
                   !max_ref.compare_exchange_weak(current_max, ms_u, std::memory_order_relaxed)) {
                // 경쟁 갱신이 있을 수 있으므로 CAS를 재시도한다.
            }
        }
        
        info("metric=wb_flush wb_commit_ms=" + std::to_string(ms) +
             " wb_batch_size=" + std::to_string(buf.size()) +
             " wb_ok_total=" + std::to_string(ok) +
             " wb_fail_total=" + std::to_string(fail) +
             " wb_dlq_total=" + std::to_string(dlqed));

        buf.clear();
    }

    void ProcessEntry(pqxx::transaction_base& tx, const server::core::storage::redis::IRedisClient::StreamEntry& e) {
        
        std::string type = "unknown";
        std::string ts_ms = "0";
        std::optional<std::string> user_id;
        std::optional<std::string> session_id;
        std::optional<std::string> room_id;
        std::string trace_id;
        std::string correlation_id;

        // 원본 field를 그대로 JSON payload로 보존해 두면, 나중에 DLQ 재처리나 감사 시 원문 복원이 쉬워진다.
        json j = json::object();
        for (const auto& f : e.fields) {
            if (f.first == "type") type = f.second;
            else if (f.first == "ts_ms") ts_ms = f.second;
            else if (f.first == "user_id") user_id = f.second;
            else if (f.first == "session_id") session_id = f.second;
            else if (f.first == "room_id") room_id = f.second;
            else if (f.first == "trace_id") trace_id = f.second;
            else if (f.first == "correlation_id") correlation_id = f.second;

            j[f.first] = f.second;
        }

        // UUID 필드는 유효한 값만 DB에 넣는다.
        // 스트림 원문이 더럽더라도 적재 단계에서 SQL 타입 오류로 전체 배치를 터뜨리지 않게 하려는 방어선이다.
        auto normalize_uuid = [](const std::optional<std::string>& opt) -> std::string {
            if (!opt || opt->empty()) return "";
            if (!IsUuid(*opt)) return "";
            return *opt;
        };

        std::string uid_v = normalize_uuid(user_id);
        std::string sid_v = normalize_uuid(session_id);
        std::string rid_v = normalize_uuid(room_id);

        long long ts_v = 0;
        try { ts_v = std::stoll(ts_ms); } catch (...) { ts_v = 0; }

        server::core::trace::ScopedContext trace_scope(trace_id, correlation_id, !trace_id.empty());
        if (trace_scope.active()) {
            server::core::log::debug("span_start component=wb_worker span=db_insert");
        }

        // DB insert는 prepared statement로 고정해 파싱 비용과 SQL 주입 면을 함께 줄인다.
        const std::string payload = j.dump();
        tx.exec_prepared("wb_insert_session_event",
                         e.id, type, ts_v, uid_v, sid_v, rid_v, payload);

        if (trace_scope.active()) {
            server::core::log::debug("span_end component=wb_worker span=db_insert success=true");
        }
    }

    bool SendToDlq(const server::core::storage::redis::IRedisClient::StreamEntry& e, const char* error_msg) {
        try {
            std::vector<std::pair<std::string, std::string>> fields;
            fields.emplace_back("orig_event_id", e.id);
            for (const auto& f : e.fields) {
                fields.emplace_back(f.first, f.second);
            }
            fields.emplace_back("error", error_msg);
            
            // DLQ는 조사 대상이므로 기본적으로 최대 길이를 강제하지 않는다.
            // 자동 trimming을 걸면 장애 순간의 원인 표본이 가장 먼저 사라질 수 있다.
            (void)redis_->xadd(config_.dlq_stream, fields, nullptr, std::nullopt, true);
            return true;
        } catch (...) {
            return false;
        }
    }

    server::core::app::EngineRuntime engine_;
    server::core::app::AppHost& app_host_;
    WorkerConfig config_;
    std::shared_ptr<server::core::storage::redis::IRedisClient> redis_;
    std::unique_ptr<pqxx::connection> db_;
    bool db_prepared_{false};

    std::atomic<long long> wb_pending_{0};
    std::atomic<bool> redis_dependency_ok_{false};

    std::string reclaim_next_id_{"0-0"};
    std::atomic<std::uint64_t> wb_reclaim_runs_total_{0};
    std::atomic<std::uint64_t> wb_reclaim_total_{0};
    std::atomic<std::uint64_t> wb_reclaim_error_total_{0};
    std::atomic<std::uint64_t> wb_reclaim_deleted_total_{0};
    std::atomic<std::uint64_t> wb_redis_unavailable_total_{0};

    std::atomic<std::uint64_t> wb_db_unavailable_total_{0};
    std::atomic<std::uint64_t> wb_error_drop_total_{0};
    std::atomic<std::uint64_t> wb_db_reconnect_backoff_ms_last_{0};
    std::atomic<std::uint64_t> wb_flush_retry_attempt_total_{0};
    std::atomic<std::uint64_t> wb_flush_retry_exhausted_total_{0};
    std::atomic<std::uint64_t> wb_flush_retry_delay_ms_last_{0};

    std::atomic<std::uint64_t> wb_ack_total_{0};
    std::atomic<std::uint64_t> wb_ack_fail_total_{0};

    std::atomic<std::uint64_t> wb_flush_total_{0};
    std::atomic<std::uint64_t> wb_flush_ok_total_{0};
    std::atomic<std::uint64_t> wb_flush_fail_total_{0};
    std::atomic<std::uint64_t> wb_flush_dlq_total_{0};
    std::atomic<std::uint64_t> wb_flush_batch_size_last_{0};
    std::atomic<std::uint64_t> wb_flush_commit_ms_last_{0};
    std::atomic<std::uint64_t> wb_flush_commit_ms_max_{0};
    std::atomic<std::uint64_t> wb_flush_commit_ms_sum_{0};
    std::atomic<std::uint64_t> wb_flush_commit_ms_count_{0};

    std::uint32_t db_reconnect_attempt_{0};
    mutable std::mt19937_64 rng_{std::random_device{}()};

    std::string RenderMetrics() const {
        std::ostringstream stream;

        // build metadata를 같이 내보내면 어느 워커 바이너리가 backlog를 처리 중인지 메트릭만으로도 바로 식별할 수 있다.
        server::core::metrics::append_build_info(stream);
        server::core::metrics::append_runtime_core_metrics(stream);
        server::core::metrics::append_prometheus_metrics(stream);

        stream << "# TYPE wb_batch_max_events gauge\n";
        stream << "wb_batch_max_events " << config_.batch_max_events << "\n";
        stream << "# TYPE wb_batch_max_bytes gauge\n";
        stream << "wb_batch_max_bytes " << config_.batch_max_bytes << "\n";
        stream << "# TYPE wb_batch_delay_ms gauge\n";
        stream << "wb_batch_delay_ms " << config_.batch_delay_ms << "\n";

        stream << "# TYPE wb_db_reconnect_base_ms gauge\n";
        stream << "wb_db_reconnect_base_ms " << config_.db_reconnect_base_ms << "\n";
        stream << "# TYPE wb_db_reconnect_max_ms gauge\n";
        stream << "wb_db_reconnect_max_ms " << config_.db_reconnect_max_ms << "\n";
        stream << "# TYPE wb_retry_max gauge\n";
        stream << "wb_retry_max " << config_.retry_max << "\n";
        stream << "# TYPE wb_retry_backoff_ms gauge\n";
        stream << "wb_retry_backoff_ms " << config_.retry_backoff_ms << "\n";

        stream << "# TYPE wb_reclaim_enabled gauge\n";
        stream << "wb_reclaim_enabled " << (config_.reclaim_enabled ? 1 : 0) << "\n";
        stream << "# TYPE wb_reclaim_interval_ms gauge\n";
        stream << "wb_reclaim_interval_ms " << config_.reclaim_interval_ms << "\n";
        stream << "# TYPE wb_reclaim_min_idle_ms gauge\n";
        stream << "wb_reclaim_min_idle_ms " << config_.reclaim_min_idle_ms << "\n";
        stream << "# TYPE wb_reclaim_count gauge\n";
        stream << "wb_reclaim_count " << config_.reclaim_count << "\n";
 
        stream << "# TYPE wb_pending gauge\n";
        stream << "wb_pending " << wb_pending_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_reclaim_runs_total counter\n";
        stream << "wb_reclaim_runs_total " << wb_reclaim_runs_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_reclaim_total counter\n";
        stream << "wb_reclaim_total " << wb_reclaim_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_reclaim_error_total counter\n";
        stream << "wb_reclaim_error_total " << wb_reclaim_error_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_reclaim_deleted_total counter\n";
        stream << "wb_reclaim_deleted_total " << wb_reclaim_deleted_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_redis_unavailable_total counter\n";
        stream << "wb_redis_unavailable_total " << wb_redis_unavailable_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_db_unavailable_total counter\n";
        stream << "wb_db_unavailable_total " << wb_db_unavailable_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_db_reconnect_backoff_ms_last gauge\n";
        stream << "wb_db_reconnect_backoff_ms_last "
               << wb_db_reconnect_backoff_ms_last_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_flush_retry_delay_ms_last gauge\n";
        stream << "wb_flush_retry_delay_ms_last "
               << wb_flush_retry_delay_ms_last_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_flush_retry_attempt_total counter\n";
        stream << "wb_flush_retry_attempt_total "
               << wb_flush_retry_attempt_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_flush_retry_exhausted_total counter\n";
        stream << "wb_flush_retry_exhausted_total "
               << wb_flush_retry_exhausted_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_error_drop_total counter\n";
        stream << "wb_error_drop_total " << wb_error_drop_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_ack_total counter\n";
        stream << "wb_ack_total " << wb_ack_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_ack_fail_total counter\n";
        stream << "wb_ack_fail_total " << wb_ack_fail_total_.load(std::memory_order_relaxed) << "\n";
 
        stream << "# TYPE wb_flush_total counter\n";
        stream << "wb_flush_total " << wb_flush_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_flush_ok_total counter\n";
        stream << "wb_flush_ok_total " << wb_flush_ok_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_flush_fail_total counter\n";
        stream << "wb_flush_fail_total " << wb_flush_fail_total_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_flush_dlq_total counter\n";
        stream << "wb_flush_dlq_total " << wb_flush_dlq_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_flush_batch_size_last gauge\n";
        stream << "wb_flush_batch_size_last " << wb_flush_batch_size_last_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE wb_flush_commit_ms_last gauge\n";
        stream << "wb_flush_commit_ms_last " << wb_flush_commit_ms_last_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_flush_commit_ms_max gauge\n";
        stream << "wb_flush_commit_ms_max " << wb_flush_commit_ms_max_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_flush_commit_ms_sum counter\n";
        stream << "wb_flush_commit_ms_sum " << wb_flush_commit_ms_sum_.load(std::memory_order_relaxed) << "\n";
        stream << "# TYPE wb_flush_commit_ms_count counter\n";
        stream << "wb_flush_commit_ms_count " << wb_flush_commit_ms_count_.load(std::memory_order_relaxed) << "\n";

        stream << app_host_.dependency_metrics_text();
        stream << app_host_.lifecycle_metrics_text();

        return stream.str();
    }
};

int main(int, char**) {
    try {
        auto config = WorkerConfig::Load();
        WbWorker worker(std::move(config));
        return worker.Run();
    } catch (const std::exception& e) {
        std::cerr << "WB worker fatal error: " << e.what() << std::endl;
        return 1;
    }
}
