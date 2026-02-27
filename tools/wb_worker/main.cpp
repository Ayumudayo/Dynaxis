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

#include "server/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
#include "server/core/app/app_host.hpp"
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

    // Pending reclaim (PEL) settings
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
        // .env 파일 로드 (기존 환경변수 덮어쓰기)

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

std::size_t EstimateEntryBytes(const server::storage::redis::IRedisClient::StreamEntry& e) {
    std::size_t est = e.id.size();
    for (const auto& f : e.fields) {
        est += f.first.size() + f.second.size() + 4;
    }
    return est;
}

} // namespace

// -----------------------------------------------------------------------------
// Write-Back Worker
// -----------------------------------------------------------------------------
/**
 * @brief Write-Behind 패턴을 구현한 워커 클래스
 * 
 * Redis Stream에 쌓인 이벤트를 비동기적으로 읽어서 PostgreSQL DB에 저장합니다.
 * 이 패턴을 사용하면 메인 서버가 DB 쓰기 부하를 직접 감당하지 않아도 되므로
 * 응답 속도가 빨라지고, 트래픽 폭주 시에도 DB를 보호할 수 있습니다.
 * 
 * 주요 흐름:
 * 1. Redis Stream (`session_events`)에서 Consumer Group을 통해 메시지를 읽습니다.
 * 2. 읽은 메시지를 내부 버퍼에 모읍니다 (Batching).
 * 3. 일정 개수(`batch_max_events`)가 모이거나 일정 시간(`batch_delay_ms`)이 지나면 DB에 저장합니다.
 * 4. 저장 성공 시 Redis에 ACK를 보내 메시지 처리를 완료합니다.
 * 5. 실패 시 DLQ(Dead Letter Queue)로 보내거나 재시도합니다.
 */
class WbWorker {
public:
    explicit WbWorker(WorkerConfig config) : config_(std::move(config)) {}

    int Run() {
        app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kBootstrapping);
        server::core::app::install_termination_signal_handlers();

        // Readiness requires both Redis and DB to function.
        app_host_.declare_dependency("redis");
        app_host_.declare_dependency("db");
        app_host_.set_dependency_ok("redis", false);
        app_host_.set_dependency_ok("db", false);
        app_host_.set_ready(false);

        if (config_.db_uri.empty()) {
            std::cerr << "WB worker: DB_URI not set" << std::endl;
            app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kFailed);
            return 2;
        }
        if (config_.redis_uri.empty()) {
            std::cerr << "WB worker: REDIS_URI not set" << std::endl;
            app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kFailed);
            return 2;
        }

        if (config_.ack_on_error && !config_.dlq_on_error) {
            server::core::log::warn(
                "WB worker configured with WB_ACK_ON_ERROR=1 and WB_DLQ_ON_ERROR=0; failed events may be dropped"
            );
        }

        // Redis 연결
        server::storage::redis::Options ropts{};
        redis_ = server::storage::redis::make_redis_client(config_.redis_uri, ropts);
        if (!redis_ || !redis_->health_check()) {
            std::cerr << "WB worker: Redis health check failed" << std::endl;
            app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kFailed);
            return 3;
        }
        app_host_.set_dependency_ok("redis", true);

        // Consumer Group 생성 (이미 존재하면 무시됨)
        // Consumer Group은 여러 워커가 메시지를 중복 없이 나눠서 처리하게 해줍니다.
        (void)redis_->xgroup_create_mkstream(config_.stream_key, config_.group);
        info("WB worker consuming stream=" + config_.stream_key + 
             ", group=" + config_.group + ", consumer=" + config_.consumer);

        // DB 연결 (초기 연결 시도)
        if (!EnsureDbConnection()) {
            std::cerr << "WB worker: Initial DB connection failed" << std::endl;
            // 초기 연결 실패 시 종료할지, 재시도할지 정책 결정 필요.
            // 여기서는 재시도 루프를 돌기 위해 일단 진행하거나, 
            // 안전하게 종료할 수 있음. 현재 요구사항은 "재연결"이므로
            // Loop() 내에서 처리하도록 함.
        }

        app_host_.start_admin_http(config_.metrics_port, [this]() { return RenderMetrics(); });
        app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kRunning);

        // 메인 루프 시작
        Loop();
        app_host_.set_ready(false);
        app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kStopped);
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

    void ResetDbReconnectBackoff() {
        db_reconnect_attempt_ = 0;
        wb_db_reconnect_backoff_ms_last_.store(0, std::memory_order_relaxed);
    }

    void SleepDbReconnectBackoff() {
        const auto capped_attempt = std::min<std::uint32_t>(db_reconnect_attempt_, 16);
        const auto base = static_cast<std::uint64_t>(std::max<long long>(1, config_.db_reconnect_base_ms));
        const auto cap = static_cast<std::uint64_t>(std::max<long long>(base, config_.db_reconnect_max_ms));
        const auto exp = std::min<std::uint64_t>(cap, base * (1ull << capped_attempt));

        std::uniform_int_distribution<std::uint64_t> dist(0, exp);
        const auto delay = dist(rng_);

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
        
        std::vector<server::storage::redis::IRedisClient::StreamEntry> buf;
        buf.reserve(config_.batch_max_events);
        std::size_t buf_bytes = 0;

        while (!app_host_.stop_requested()) {
            // DB 연결 확인 (끊어졌으면 재연결 시도)
            if (!EnsureDbConnection()) {
                app_host_.set_ready(false);
                SleepDbReconnectBackoff();
                continue;
            }

            ResetDbReconnectBackoff();
            app_host_.set_ready(true);

            // 0. Pending reclaim (PEL)
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

            // 1. Redis에서 메시지 읽기 (Blocking)
            std::vector<server::storage::redis::IRedisClient::StreamEntry> entries;
            if (!redis_->xreadgroup(config_.stream_key, config_.group, config_.consumer, 
                                  500, config_.batch_max_events, entries)) {
                // 타임아웃 또는 에러 시 잠시 대기
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                // 에러 상황일 수 있으므로 continue
                continue; 
            }

            // 2. 버퍼링
            if (!entries.empty()) {
                for (auto& e : entries) {
                    buf_bytes += EstimateEntryBytes(e);
                    buf.emplace_back(std::move(e));

                    // 배치 크기나 용량 초과 시 즉시 플러시
                    if (buf.size() >= config_.batch_max_events || buf_bytes >= config_.batch_max_bytes) {
                        Flush(buf);
                        buf_bytes = 0;
                        last_flush = std::chrono::steady_clock::now();
                    }
                }
            }

            // 3. 시간 기반 플러시 (데이터가 적어도 일정 시간 지나면 저장)
            auto now = std::chrono::steady_clock::now();
            if (!buf.empty() && 
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= config_.batch_delay_ms) {
                Flush(buf);
                buf_bytes = 0;
                last_flush = now;
            }

            // 4. Pending 상태 모니터링 (1초마다)
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pending_log).count() >= 1000) {
                long long pending = 0;
                if (redis_->xpending(config_.stream_key, config_.group, pending)) {
                    wb_pending_.store(pending, std::memory_order_relaxed);
                    server::core::log::info("metric=wb_pending value=" + std::to_string(pending));
                }
                last_pending_log = now;
            }
        }
    }

    void ReclaimPending(long long min_idle_ms,
                        std::vector<server::storage::redis::IRedisClient::StreamEntry>& buf,
                        std::size_t& buf_bytes,
                        std::chrono::steady_clock::time_point& last_flush) {
        if (!redis_) {
            return;
        }

        server::storage::redis::IRedisClient::StreamAutoClaimResult claimed;
        wb_reclaim_runs_total_.fetch_add(1, std::memory_order_relaxed);
        if (!redis_->xautoclaim(config_.stream_key,
                                config_.group,
                                config_.consumer,
                                min_idle_ms,
                                reclaim_next_id_,
                                config_.reclaim_count,
                                claimed)) {
            wb_reclaim_error_total_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

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

    // 버퍼에 쌓인 이벤트를 DB에 저장하고 Redis에 ACK 처리
    //
    // 동작 방식:
    // 1) 배치 단위로 하나의 트랜잭션을 열고, 이벤트 단위 실패는 savepoint(subtransaction)로 격리한다.
    //    - 개별 트랜잭션(1 event = 1 transaction)보다 훨씬 빠르면서도,
    //      특정 이벤트의 포맷 오류가 전체 배치를 망치지 않도록 한다.
    // 2) 트랜잭션 commit 성공 후에만 ACK 한다. (At-least-once)
    // 3) 영구적 오류는 DLQ로 이동한 뒤 ACK(옵션)하여 무한 재시도를 방지한다.
    void Flush(std::vector<server::storage::redis::IRedisClient::StreamEntry>& buf) {
        if (buf.empty()) return;

        auto t0 = std::chrono::steady_clock::now();
        std::size_t ok = 0;
        std::size_t fail = 0;
        std::size_t dlqed = 0;

        bool committed = false;
        for (std::uint32_t attempt = 0; attempt <= config_.retry_max && !committed; ++attempt) {
            if (attempt > 0) {
                wb_flush_retry_attempt_total_.fetch_add(1, std::memory_order_relaxed);

                const auto delay_ms = std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(config_.retry_backoff_ms) * attempt,
                    30000ull
                );
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
                // retry
            }
        }
        
        info("metric=wb_flush wb_commit_ms=" + std::to_string(ms) +
             " wb_batch_size=" + std::to_string(buf.size()) +
             " wb_ok_total=" + std::to_string(ok) +
             " wb_fail_total=" + std::to_string(fail) +
             " wb_dlq_total=" + std::to_string(dlqed));

        buf.clear();
    }

    void ProcessEntry(pqxx::transaction_base& tx, const server::storage::redis::IRedisClient::StreamEntry& e) {
        
        std::string type = "unknown";
        std::string ts_ms = "0";
        std::optional<std::string> user_id;
        std::optional<std::string> session_id;
        std::optional<std::string> room_id;

        // JSON Payload 생성 (nlohmann/json 사용)
        json j = json::object();
        for (const auto& f : e.fields) {
            if (f.first == "type") type = f.second;
            else if (f.first == "ts_ms") ts_ms = f.second;
            else if (f.first == "user_id") user_id = f.second;
            else if (f.first == "session_id") session_id = f.second;
            else if (f.first == "room_id") room_id = f.second;

            j[f.first] = f.second;
        }

        // UUID 검증 및 정규화
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

        // DB Insert
        const std::string payload = j.dump();
        tx.exec_prepared("wb_insert_session_event",
                         e.id, type, ts_v, uid_v, sid_v, rid_v, payload);
    }

    bool SendToDlq(const server::storage::redis::IRedisClient::StreamEntry& e, const char* error_msg) {
        try {
            std::vector<std::pair<std::string, std::string>> fields;
            fields.emplace_back("orig_event_id", e.id);
            for (const auto& f : e.fields) {
                fields.emplace_back(f.first, f.second);
            }
            fields.emplace_back("error", error_msg);
            
            // DLQ 스트림에 추가 (최대 길이 제한 없음)
            (void)redis_->xadd(config_.dlq_stream, fields, nullptr, std::nullopt, true);
            return true;
        } catch (...) {
            return false;
        }
    }

    server::core::app::AppHost app_host_{"wb_worker"};

    WorkerConfig config_;
    std::shared_ptr<server::storage::redis::IRedisClient> redis_;
    std::unique_ptr<pqxx::connection> db_;
    bool db_prepared_{false};

    std::atomic<long long> wb_pending_{0};

    std::string reclaim_next_id_{"0-0"};
    std::atomic<std::uint64_t> wb_reclaim_runs_total_{0};
    std::atomic<std::uint64_t> wb_reclaim_total_{0};
    std::atomic<std::uint64_t> wb_reclaim_error_total_{0};
    std::atomic<std::uint64_t> wb_reclaim_deleted_total_{0};

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

        // Build metadata (git hash/describe + build time)
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
