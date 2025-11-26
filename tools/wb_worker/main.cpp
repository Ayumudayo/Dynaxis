// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>
#include <atomic>

#include "server/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
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

    static WorkerConfig Load() {
        WorkerConfig cfg;
        // .env 파일 로드 (기존 환경변수 덮어쓰기)

        if (const char* v = std::getenv("DB_URI")) cfg.db_uri = v;
        if (const char* v = std::getenv("REDIS_URI")) cfg.redis_uri = v;
        if (const char* v = std::getenv("REDIS_STREAM_KEY")) cfg.stream_key = v;
        if (const char* v = std::getenv("WB_GROUP")) cfg.group = v;
        if (const char* v = std::getenv("WB_CONSUMER")) cfg.consumer = v;
        if (const char* v = std::getenv("WB_DLQ_STREAM")) cfg.dlq_stream = v;

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

} // namespace

// -----------------------------------------------------------------------------
// Write-Back Worker
// -----------------------------------------------------------------------------
class WbWorker {
public:
    explicit WbWorker(WorkerConfig config) : config_(std::move(config)) {}

    int Run() {
        if (config_.db_uri.empty()) {
            std::cerr << "WB worker: DB_URI not set" << std::endl;
            return 2;
        }
        if (config_.redis_uri.empty()) {
            std::cerr << "WB worker: REDIS_URI not set" << std::endl;
            return 2;
        }

        // Redis 연결
        server::storage::redis::Options ropts{};
        redis_ = server::storage::redis::make_redis_client(config_.redis_uri, ropts);
        if (!redis_ || !redis_->health_check()) {
            std::cerr << "WB worker: Redis health check failed" << std::endl;
            return 3;
        }

        // Consumer Group 생성 (이미 존재하면 무시됨)
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

        // 메인 루프
        Loop();
        return 0;
    }

private:
    bool EnsureDbConnection() {
        if (db_ && db_->is_open()) return true;

        try {
            db_ = std::make_unique<pqxx::connection>(config_.db_uri);
            if (db_->is_open()) {
                info("DB connected successfully.");
                return true;
            }
        } catch (const std::exception& e) {
            std::cerr << "DB connection attempt failed: " << e.what() << std::endl;
        }
        return false;
    }

    void Loop() {
        auto last_flush = std::chrono::steady_clock::now();
        auto last_pending_log = std::chrono::steady_clock::now();
        
        std::vector<server::storage::redis::IRedisClient::StreamEntry> buf;
        buf.reserve(config_.batch_max_events);
        std::size_t buf_bytes = 0;

        while (true) {
            // DB 연결 확인 (끊어졌으면 재연결 시도)
            if (!EnsureDbConnection()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
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
                    std::size_t est = e.id.size();
                    for (const auto& f : e.fields) {
                        est += f.first.size() + f.second.size() + 4;
                    }
                    buf_bytes += est;
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
                    server::core::log::info("metric=wb_pending value=" + std::to_string(pending));
                }
                last_pending_log = now;
            }
        }
    }

    // 버퍼에 쌓인 이벤트를 DB에 저장하고 Redis에 ACK 처리
    //
    // 동작 방식:
    // 1. 버퍼에 있는 모든 이벤트를 순회하며 DB 트랜잭션을 개별적으로 수행합니다.
    //    - 배치 단위 트랜잭션이 아닌 개별 트랜잭션을 사용하는 이유는,
    //      하나의 이벤트 실패가 전체 배치의 롤백을 유발하지 않게 하기 위함입니다.
    // 2. DB 저장이 성공하면 Redis Stream에 ACK를 보내 해당 메시지가 처리되었음을 알립니다.
    // 3. 실패 시:
    //    - DLQ(Dead Letter Queue) 사용이 활성화되어 있으면, 실패한 이벤트를 DLQ 스트림으로 이동시킵니다.
    //    - DLQ 이동에 성공하면 원본 스트림에서는 ACK 처리하여 중복 처리를 방지합니다.
    //    - DLQ 이동조차 실패하거나 DLQ를 사용하지 않는 경우, 설정에 따라 ACK를 수행하여
    //      무한 재시도 루프에 빠지는 것을 방지할 수 있습니다.
    void Flush(std::vector<server::storage::redis::IRedisClient::StreamEntry>& buf) {
        if (buf.empty()) return;

        auto t0 = std::chrono::steady_clock::now();
        std::size_t ok = 0;
        std::size_t fail = 0;
        std::size_t dlqed = 0;

        for (const auto& e : buf) {
            try {
                ProcessEntry(e);
                // 성공 시 ACK: Redis에게 이 메시지는 안전하게 처리되었음을 알림
                (void)redis_->xack(config_.stream_key, config_.group, e.id);
                ++ok;
            } catch (const std::exception& ex) {
                // DB 연결 끊김 예외인 경우, 즉시 재시도하지 않고 루프의 다음 턴에서 재연결을 시도하도록
                // 예외를 던지거나, 여기서 재연결을 시도할 수 있습니다.
                // 현재 구조상 pqxx::broken_connection 예외가 발생하면 db_ 객체가 유효하지 않게 되므로
                // EnsureDbConnection()이 호출되는 다음 루프까지 기다리는 것이 안전합니다.
                // 다만, 이 경우 ACK를 하지 않으므로 다음 번에 다시 읽혀서 처리됩니다 (At-least-once).
                
                // 만약 데이터 포맷 에러 등 '영구적 오류'라면 DLQ로 보내야 합니다.
                // pqxx::broken_connection은 일시적 오류로 간주해야 합니다.
                if (dynamic_cast<const pqxx::broken_connection*>(&ex)) {
                    std::cerr << "DB connection broken during flush. Will retry later." << std::endl;
                    db_.reset(); // 연결 객체 해제하여 재연결 유도
                    continue; // ACK 하지 않고 넘어감 -> 재처리 대상
                }

                ++fail;
                bool acked = false;
                
                // 에러 발생 시 DLQ로 이동 시도
                // 이는 일시적인 DB 오류가 아닌 데이터 포맷 문제 등의 영구적 오류일 때 중요합니다.
                if (config_.dlq_on_error && !config_.dlq_stream.empty()) {
                    if (SendToDlq(e, ex.what())) {
                        // DLQ 이동 성공 시 원본 스트림에서는 ACK 처리
                        (void)redis_->xack(config_.stream_key, config_.group, e.id);
                        acked = true;
                        ++dlqed;
                    }
                }

                // DLQ 이동 실패했거나 DLQ 미사용 시에도 설정에 따라 ACK 처리 (무한 루프 방지)
                if (!acked && config_.ack_on_error) {
                    (void)redis_->xack(config_.stream_key, config_.group, e.id);
                }
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        
        info("metric=wb_flush wb_commit_ms=" + std::to_string(ms) +
             " wb_batch_size=" + std::to_string(buf.size()) +
             " wb_ok_total=" + std::to_string(ok) +
             " wb_fail_total=" + std::to_string(fail) +
             " wb_dlq_total=" + std::to_string(dlqed));

        buf.clear();
    }

    void ProcessEntry(const server::storage::redis::IRedisClient::StreamEntry& e) {
        if (!db_ || !db_->is_open()) {
            throw std::runtime_error("DB connection not open");
        }
        pqxx::work w(*db_);
        
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
        w.exec_params(
            "insert into session_events(event_id, type, ts, user_id, session_id, room_id, payload) "
            "values ($1, $2, to_timestamp(($3)::bigint/1000.0), "
            "nullif($4,'')::uuid, nullif($5,'')::uuid, nullif($6,'')::uuid, $7::jsonb) "
            "on conflict (event_id) do nothing",
            e.id, type, ts_v, uid_v, sid_v, rid_v, j.dump()
        );
        w.commit();
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

    WorkerConfig config_;
    std::shared_ptr<server::storage::redis::IRedisClient> redis_;
    std::unique_ptr<pqxx::connection> db_;
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
