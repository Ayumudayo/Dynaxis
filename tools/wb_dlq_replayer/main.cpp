// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <algorithm>

#include "../wb_common/redis_client_factory.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
#include <pqxx/pqxx>
#include <nlohmann/json.hpp>

using server::core::log::info;
using json = nlohmann::json;

/**
 * @brief DLQ 스트림 재처리 도구 구현입니다.
 *
 * DLQ 이벤트를 재삽입하고 실패 시 재시도/Dead stream 격리를 수행해,
 * write-behind 장애 복구를 운영자가 통제 가능한 절차로 유지합니다.
 */
namespace {

// -----------------------------------------------------------------------------
// 설정 (Configuration)
// -----------------------------------------------------------------------------
struct ReplayerConfig {
    std::string db_uri;
    std::string redis_uri;
    std::string dlq_stream = "session_events_dlq";
    std::string dead_stream = "session_events_dead";
    std::string group = "wb_dlq_group";
    std::string consumer = "wb_dlq_consumer";
    
    unsigned int retry_max = 5;
    long long retry_backoff_ms = 250;

    static ReplayerConfig Load() {
        ReplayerConfig cfg;

        if (const char* v = std::getenv("DB_URI")) cfg.db_uri = v;
        if (const char* v = std::getenv("REDIS_URI")) cfg.redis_uri = v;
        if (const char* v = std::getenv("WB_DLQ_STREAM")) cfg.dlq_stream = v;
        if (const char* v = std::getenv("WB_DEAD_STREAM")) cfg.dead_stream = v;
        if (const char* v = std::getenv("WB_GROUP_DLQ")) cfg.group = v;
        if (const char* v = std::getenv("WB_CONSUMER")) cfg.consumer = v;

        if (const char* v = std::getenv("WB_RETRY_MAX")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n > 0 && n < 1000) cfg.retry_max = static_cast<unsigned int>(n);
        }
        if (const char* v = std::getenv("WB_RETRY_BACKOFF_MS")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n >= 50 && n <= 60000) cfg.retry_backoff_ms = static_cast<long long>(n);
        }
        if (const char* v = std::getenv("WB_DLQ_BATCH")) {
            auto n = std::strtoul(v, nullptr, 10);
            if (n > 0 && n <= 1000) cfg.batch_size = static_cast<int>(n);
        }
        return cfg;
    }

    int batch_size = 50; // Default from docs
};

// -----------------------------------------------------------------------------
// 유틸리티 (Utilities)
// -----------------------------------------------------------------------------
std::unordered_map<std::string, std::string> ToMap(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::unordered_map<std::string, std::string> m;
    m.reserve(fields.size());
    for (const auto& kv : fields) m.emplace(kv.first, kv.second);
    return m;
}

} // namespace

// -----------------------------------------------------------------------------
// DLQ Replayer
// -----------------------------------------------------------------------------
class WbDlqReplayer {
public:
    explicit WbDlqReplayer(ReplayerConfig config) : config_(std::move(config)) {}

    /**
     * @brief DLQ 리플레이 루프를 실행합니다.
     * @param run_once true면 배치를 한 번만 처리하고 종료
     * @return 종료 코드(0 정상)
     */
    int Run(bool run_once) {
        if (config_.db_uri.empty()) {
            std::cerr << "DLQ: DB_URI not set" << std::endl;
            return 2;
        }
        if (config_.redis_uri.empty()) {
            std::cerr << "DLQ: REDIS_URI not set" << std::endl;
            return 2;
        }

        // Redis 연결
        server::core::storage::redis::Options ropts{};
        redis_ = wb_tools::make_redis_client(config_.redis_uri, ropts);
        if (!redis_ || !redis_->health_check()) {
            std::cerr << "DLQ: Redis health check failed" << std::endl;
            return 3;
        }

        // Consumer Group 생성
        (void)redis_->xgroup_create_mkstream(config_.dlq_stream, config_.group);
        info("DLQ replayer consuming stream=" + config_.dlq_stream + 
             ", group=" + config_.group + ", consumer=" + config_.consumer);

        // DB 연결 (초기 연결 시도)
        if (!EnsureDbConnection()) {
            std::cerr << "DLQ: Initial DB connection failed" << std::endl;
            // 초기 연결 실패 시 종료할지, 재시도할지 정책 결정 필요.
            // 여기서는 재시도 루프를 돌기 위해 일단 진행하거나, 
            // 안전하게 종료할 수 있음. 현재 요구사항은 "재연결"이므로
            // Loop() 내에서 처리하도록 함.
        }

        Loop(run_once);
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

    void Loop(bool run_once) {
        while (true) {
            // DB 연결 확인 (끊어졌으면 재연결 시도)
            if (!EnsureDbConnection()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            std::vector<server::core::storage::redis::IRedisClient::StreamEntry> entries;
            // DLQ는 처리량이 적으므로 1초 대기
            // 문서에 명시된 WB_DLQ_BATCH 값(기본 50)을 사용
            if (!redis_->xreadgroup(config_.dlq_stream, config_.group, config_.consumer, 
                                  1000, config_.batch_size, entries)) {
                if (run_once) break; // --once 모드에서는 타임아웃/빈 응답 시 종료
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (entries.empty()) {
                if (run_once) break; // --once 모드에서는 더 이상 읽을 게 없으면 종료
            }

            for (const auto& e : entries) {
                ProcessEntry(e);
            }
            
            if (run_once && entries.size() < static_cast<size_t>(config_.batch_size)) {
                // --once 모드이고, 읽어온 개수가 배치 크기보다 작으면 
                // 더 이상 처리할 항목이 없다고 판단하여 종료
                break;
            }
        }
    }

    void ProcessEntry(const server::core::storage::redis::IRedisClient::StreamEntry& e) {
        auto mp = ToMap(e.fields);
        
        // 원본 이벤트 ID 복원 (없으면 현재 ID 사용)
        std::string orig_id = (mp.count("orig_event_id") ? mp["orig_event_id"] : e.id);
        
        // 재시도 횟수 확인
        int retry_count = 0;
        if (mp.count("retry_count")) {
            try { retry_count = std::stoi(mp["retry_count"]); } catch (...) {}
        }

        try {
            TryInsertToDb(orig_id, mp);
            
            // 성공 시 DLQ에서 ACK (제거)
            (void)redis_->xack(config_.dlq_stream, config_.group, e.id);
            info("metric=wb_dlq_replay ok=1 event_id=" + orig_id);

        } catch (const std::exception& ex) {
            // DB 연결 끊김 예외인 경우, 즉시 재시도하지 않고 루프의 다음 턴에서 재연결을 시도하도록
            // 예외를 던지거나, 여기서 재연결을 시도할 수 있습니다.
            if (dynamic_cast<const pqxx::broken_connection*>(&ex)) {
                std::cerr << "DB connection broken during replay. Will retry later." << std::endl;
                db_.reset(); // 연결 객체 해제하여 재연결 유도
                return; // ACK 하지 않고 리턴 -> 재처리 대상
            }

            HandleFailure(e, orig_id, retry_count, ex.what());
        }
    }

    void TryInsertToDb(const std::string& event_id, const std::unordered_map<std::string, std::string>& mp) {
        if (!db_ || !db_->is_open()) {
            throw std::runtime_error("DB connection not open");
        }
        pqxx::work w(*db_);

        std::string type = mp.count("type") ? mp.at("type") : "unknown";
        std::string ts_ms = mp.count("ts_ms") ? mp.at("ts_ms") : "0";
        std::string uid = mp.count("user_id") ? mp.at("user_id") : "";
        std::string sid = mp.count("session_id") ? mp.at("session_id") : "";
        std::string rid = mp.count("room_id") ? mp.at("room_id") : "";

        long long ts_v = 0;
        try { ts_v = std::stoll(ts_ms); } catch (...) { ts_v = 0; }

        // Payload 재구성 (DLQ 메타데이터 제외) using nlohmann/json
        json j = json::object();
        for (const auto& kv : mp) {
            if (kv.first == "orig_event_id" || kv.first == "error" || kv.first == "retry_count") continue;
            j[kv.first] = kv.second;
        }

        w.exec_params(
            "insert into session_events(event_id, type, ts, user_id, session_id, room_id, payload) "
            "values ($1, $2, to_timestamp(($3)::bigint/1000.0), "
            "nullif($4,'')::uuid, nullif($5,'')::uuid, nullif($6,'')::uuid, $7::jsonb) "
            "on conflict (event_id) do nothing",
            event_id, type, ts_v, uid, sid, rid, j.dump()
        );
        w.commit();
    }

    void HandleFailure(const server::storage::redis::IRedisClient::StreamEntry& e, 
                       const std::string& orig_id, int retry_count, const char* error_msg) {
        
        // 재시도 한도 초과 -> Dead Letter Stream으로 이동
        if (retry_count + 1 >= static_cast<int>(config_.retry_max)) {
            if (MoveToDeadStream(e, orig_id, error_msg)) {
                // 이동 성공 시에만 ACK
                (void)redis_->xack(config_.dlq_stream, config_.group, e.id);
            } else {
                // 이동 실패 시 로그만 남기고 ACK 생략 -> 다음 번에 재시도
                std::cerr << "Failed to move to dead stream. Keeping in DLQ. event_id=" << orig_id << std::endl;
            }
        } else {
            // 지수 백오프 후 DLQ에 다시 추가 (재시도 횟수 증가)
            RetryLater(e, orig_id, retry_count, error_msg);
        }
    }

    bool MoveToDeadStream(const server::storage::redis::IRedisClient::StreamEntry& e, 
                          const std::string& orig_id, const char* error_msg) {
        try {
            std::vector<std::pair<std::string, std::string>> fields;
            fields.emplace_back("orig_event_id", orig_id);
            for (const auto& kv : e.fields) {
                if (kv.first != "retry_count") fields.emplace_back(kv.first, kv.second);
            }
            fields.emplace_back("error", error_msg);
            
            return redis_->xadd(config_.dead_stream, fields, nullptr, std::nullopt, true);
        } catch (...) {
            return false;
        }
    }

    void RetryLater(const server::storage::redis::IRedisClient::StreamEntry& e, 
                    const std::string& orig_id, int retry_count, const char* error_msg) {
        try {
            // 지수 백오프 (최대 10초)
            long long delay = config_.retry_backoff_ms;
            for (int i = 0; i < retry_count; ++i) {
                if (delay < 10000) delay = std::min(delay * 2, 10000LL);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));

            std::vector<std::pair<std::string, std::string>> fields;
            fields.emplace_back("orig_event_id", orig_id);
            for (const auto& kv : e.fields) {
                if (kv.first != "retry_count") fields.emplace_back(kv.first, kv.second);
            }
            fields.emplace_back("retry_count", std::to_string(retry_count + 1));
            fields.emplace_back("error", error_msg);

            if (redis_->xadd(config_.dlq_stream, fields, nullptr, std::nullopt, true)) {
                // 성공 시에만 원본 ACK
                (void)redis_->xack(config_.dlq_stream, config_.group, e.id);
                info("metric=wb_dlq_replay retry=1 event_id=" + orig_id + " retry_count=" + std::to_string(retry_count + 1));
            } else {
                 std::cerr << "Failed to schedule retry (xadd failed): " << orig_id << std::endl;
            }
        } catch (...) {
            std::cerr << "Failed to schedule retry: " << orig_id << std::endl;
        }
    }

    ReplayerConfig config_;
    std::shared_ptr<server::core::storage::redis::IRedisClient> redis_;
    std::unique_ptr<pqxx::connection> db_;
};

int main(int argc, char** argv) {
    try {
        bool run_once = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--once") {
                run_once = true;
            }
        }

        auto config = ReplayerConfig::Load();
        WbDlqReplayer replayer(std::move(config));
        return replayer.Run(run_once);
    } catch (const std::exception& e) {
        std::cerr << "DLQ replayer fatal error: " << e.what() << std::endl;
        return 1;
    }
}
