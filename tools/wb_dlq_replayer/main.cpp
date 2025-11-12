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

#include "server/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
#include "server/core/config/dotenv.hpp"
#include <pqxx/pqxx>

using server::core::log::info;

// Redis Stream 엔트리를 map으로 변환해 JSON/SQL 직렬화 시 키를 빠르게 조회한다.
static std::unordered_map<std::string, std::string>
to_map(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::unordered_map<std::string, std::string> m; m.reserve(fields.size());
    for (auto& kv : fields) m.emplace(kv.first, kv.second);
    return m;
}

int main(int, char**) {
    try {
        if (server::core::config::load_dotenv(".env", true)) {
            info("Loaded .env for wb_dlq_replayer (override existing env = true)");
        }
        const char* dburi = std::getenv("DB_URI");
        const char* ruri = std::getenv("REDIS_URI");
        if (!dburi || !*dburi) { std::cerr << "DLQ: DB_URI not set" << std::endl; return 2; }
        if (!ruri || !*ruri) { std::cerr << "DLQ: REDIS_URI not set" << std::endl; return 2; }
        std::string dlq = std::getenv("WB_DLQ_STREAM") ? std::getenv("WB_DLQ_STREAM") : std::string("session_events_dlq");
        std::string dead = std::getenv("WB_DEAD_STREAM") ? std::getenv("WB_DEAD_STREAM") : std::string("session_events_dead");
        std::string out = std::getenv("REDIS_STREAM_KEY") ? std::getenv("REDIS_STREAM_KEY") : std::string("session_events");
        std::string group = std::getenv("WB_GROUP_DLQ") ? std::getenv("WB_GROUP_DLQ") : std::string("wb_dlq_group");
        std::string consumer = std::getenv("WB_CONSUMER") ? std::getenv("WB_CONSUMER") : std::string("wb_dlq_consumer");
        unsigned int retry_max = 5; if (const char* v = std::getenv("WB_RETRY_MAX")) { unsigned long n = std::strtoul(v, nullptr, 10); if (n>0 && n<1000) retry_max = static_cast<unsigned int>(n); }
        long long retry_backoff_ms = 250; if (const char* v = std::getenv("WB_RETRY_BACKOFF_MS")) { auto n = std::strtoul(v, nullptr, 10); if (n>=50 && n<=60000) retry_backoff_ms = static_cast<long long>(n); }

        server::storage::redis::Options ropts{};
        auto redis = server::storage::redis::make_redis_client(ruri, ropts);
        if (!redis || !redis->health_check()) { std::cerr << "DLQ: Redis health check failed" << std::endl; return 3; }
        (void)redis->xgroup_create_mkstream(dlq, group);
        info(std::string("DLQ replayer consuming stream=") + dlq + ", group=" + group + ", consumer=" + consumer);

        pqxx::connection db(dburi);
        if (!db.is_open()) { std::cerr << "DLQ: DB open failed" << std::endl; return 4; }

        auto esc_json = [](const std::string& s){ std::string o; o.reserve(s.size()+8); for (char c: s){ if (c=='"'||c=='\\') o.push_back('\\'); o.push_back(c);} return o; };

        // DLQ → (재시도/DEAD) 파이프라인을 무한 루프로 돌리며, 각 이벤트는 개별 트랜잭션으로 처리한다.
        while (true) {
            std::vector<server::storage::redis::IRedisClient::StreamEntry> entries;
            if (!redis->xreadgroup(dlq, group, consumer, 1000, 100, entries)) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            for (auto& e : entries) {
                auto mp = to_map(e.fields);
                std::string orig_id = (mp.count("orig_event_id") ? mp["orig_event_id"] : e.id);
                std::string type = mp.count("type") ? mp["type"] : std::string("unknown");
                std::string ts_ms = mp.count("ts_ms") ? mp["ts_ms"] : std::string("0");
                std::string uid = mp.count("user_id") ? mp["user_id"] : std::string();
                std::string sid = mp.count("session_id") ? mp["session_id"] : std::string();
                std::string rid = mp.count("room_id") ? mp["room_id"] : std::string();
                int retry = 0; if (mp.count("retry_count")) { try { retry = std::stoi(mp["retry_count"]); } catch (...) {} }

                try {
                    long long ts_v = 0; try { ts_v = std::stoll(ts_ms); } catch (...) { ts_v = 0; }
                    // payload: DLQ의 원본 필드에서 운영용 필드 제거 후 JSON 직렬화
                    std::ostringstream js; js << '{'; bool first=true;
                    for (auto& kv : mp) {
                        if (kv.first == "orig_event_id" || kv.first == "error" || kv.first == "retry_count") continue;
                        if (!first) js << ','; first=false;
                        js << '"' << esc_json(kv.first) << '"' << ':' << '"' << esc_json(kv.second) << '"';
                    }
                    js << '}';
                    pqxx::work w(db);
                    w.exec_params(
                        "insert into session_events(event_id, type, ts, user_id, session_id, room_id, payload) "
                        "values ($1, $2, to_timestamp(($3)::bigint/1000.0), nullif($4,'')::uuid, nullif($5,'')::uuid, nullif($6,'')::uuid, $7::jsonb) "
                        "on conflict (event_id) do nothing",
                        orig_id, type, ts_v, uid, sid, rid, js.str()
                    );
                    w.commit();
                    // 성공적으로 적재했으면 DLQ에서 ACK하여 중복 처리를 방지한다.
                    (void)redis->xack(dlq, group, e.id);
                    info(std::string("metric=wb_dlq_replay ok=1 event_id=") + orig_id);
                } catch (const std::exception& ex) {
                    // 재시도: 한도 초과 시 데드 스트림으로 이동
                    if (retry + 1 >= static_cast<int>(retry_max)) {
                        try {
                            std::vector<std::pair<std::string,std::string>> fields;
                            fields.emplace_back("orig_event_id", orig_id);
                            for (const auto& kv : e.fields) { if (kv.first != "retry_count") fields.emplace_back(kv.first, kv.second); }
                            fields.emplace_back("error", ex.what());
                            (void)redis->xadd(dead, fields, nullptr, std::nullopt, true);
                            (void)redis->xack(dlq, group, e.id);
                            info(std::string("metric=wb_dlq_replay_dead move=1 event_id=") + orig_id);
                        } catch (...) {}
                    } else {
                        try {
                            // 지수 백오프 대기(최대 10초)
                            long long delay = retry_backoff_ms;
                            for (int i = 0; i < retry; ++i) { if (delay < 10000) delay = std::min(delay * 2, 10000LL); }
                            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                            std::vector<std::pair<std::string,std::string>> fields;
                            fields.emplace_back("orig_event_id", orig_id);
                            for (const auto& kv : e.fields) { if (kv.first != "retry_count") fields.emplace_back(kv.first, kv.second); }
                            fields.emplace_back("retry_count", std::to_string(retry + 1));
                            fields.emplace_back("error", ex.what());
                            (void)redis->xadd(dlq, fields, nullptr, std::nullopt, true);
                            (void)redis->xack(dlq, group, e.id);
                            info(std::string("metric=wb_dlq_replay retry=1 event_id=") + orig_id + " retry_count=" + std::to_string(retry + 1));
                        } catch (...) {}
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "DLQ replayer error: " << e.what() << std::endl; return 1;
    }
}
