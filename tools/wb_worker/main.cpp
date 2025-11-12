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
#include "server/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
// .env 로더
#include "server/core/config/dotenv.hpp"
#include <pqxx/pqxx>

// wb_worker는 Redis Stream → PostgreSQL write-behind 경로를 담당한다.
int main(int, char**) {
    using server::core::log::info;
    try {
        // .env가 있으면 이를 우선(override=true), 없으면 OS 환경변수 사용
        if (server::core::config::load_dotenv(".env", true)) {
            info("Loaded .env for wb_worker (override existing env = true)");
        }
        const char* dburi = std::getenv("DB_URI");
        if (!dburi || !*dburi) {
            std::cerr << "WB worker: DB_URI not set" << std::endl;
            return 2;
        }
        const char* ruri = std::getenv("REDIS_URI");
        if (!ruri || !*ruri) {
            std::cerr << "WB worker: REDIS_URI not set" << std::endl;
            return 2;
        }
        server::storage::redis::Options ropts{};
        auto redis = server::storage::redis::make_redis_client(ruri, ropts);
        if (!redis || !redis->health_check()) { std::cerr << "WB worker: Redis health check failed" << std::endl; return 3; }
        std::string stream = std::getenv("REDIS_STREAM_KEY") ? std::getenv("REDIS_STREAM_KEY") : std::string("session_events");
        std::string group  = std::getenv("WB_GROUP") ? std::getenv("WB_GROUP") : std::string("wb_group");
        std::string consumer = std::getenv("WB_CONSUMER") ? std::getenv("WB_CONSUMER") : std::string("wb_consumer");
        // 배치 임계치(개수/바이트/지연)를 env로 조정 가능하게 해 운영 환경에 맞게 튜닝한다.
        std::size_t batch_max = 100; if (const char* v = std::getenv("WB_BATCH_MAX_EVENTS")) { auto n = std::strtoul(v, nullptr, 10); if (n>0 && n<=10000) batch_max = static_cast<std::size_t>(n); }
        std::size_t batch_max_bytes = 512*1024; if (const char* v = std::getenv("WB_BATCH_MAX_BYTES")) { auto n = std::strtoul(v, nullptr, 10); if (n>=16*1024 && n<=16*1024*1024) batch_max_bytes = static_cast<std::size_t>(n); }
        long long batch_delay_ms = 500; if (const char* v = std::getenv("WB_BATCH_DELAY_MS")) { auto n = std::strtoul(v, nullptr, 10); if (n>=50 && n<=10000) batch_delay_ms = static_cast<long long>(n); }
        (void)redis->xgroup_create_mkstream(stream, group);
        info(std::string("WB worker consuming stream=") + stream + ", group=" + group + ", consumer=" + consumer);
        std::string dlq = std::getenv("WB_DLQ_STREAM") ? std::getenv("WB_DLQ_STREAM") : std::string("session_events_dlq");
        bool dlq_on_err = true; if (const char* v = std::getenv("WB_DLQ_ON_ERROR")) { dlq_on_err = std::string(v) != "0"; }
        bool ack_on_err = true; if (const char* v = std::getenv("WB_ACK_ON_ERROR")) { ack_on_err = std::string(v) != "0"; }
        // DB 연결은 배치 단위로 새로 열어도 되지만, 여기선 연결 유지
        pqxx::connection db(dburi);
        if (!db.is_open()) { std::cerr << "WB worker: DB open failed" << std::endl; return 4; }

        auto last_flush = std::chrono::steady_clock::now();
        auto last_pending_log = std::chrono::steady_clock::now();
        std::vector<server::storage::redis::IRedisClient::StreamEntry> buf;
        buf.reserve(batch_max);
        std::size_t buf_bytes = 0;

        auto is_uuid = [](const std::string& s) -> bool {
            if (s.size() != 36) return false;
            auto hex = [](char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
            const int dash[4] = {8,13,18,23};
            for (int i=0,j=0;i<36;++i){
                if (j<4 && i==dash[j]) { if (s[i] != '-') return false; ++j; }
                else if (!hex(s[i])) return false;
            }
            return true;
        };

        // flush()는 Redis로부터 수집한 StreamEntry를 PostgreSQL에 기록하고, 실패 시 DLQ로 넘긴다.
        auto flush = [&](bool /*force*/){
            if (buf.empty()) return true;
            auto t0 = std::chrono::steady_clock::now();
            std::size_t ok = 0; std::size_t fail = 0; std::size_t dlqed = 0;
            for (const auto& e : buf) {
                try {
                    // 개별 트랜잭션 처리(부분 커밋 허용)
                    pqxx::work w(db);
                    std::string type; std::string ts_ms; std::optional<std::string> user_id; std::optional<std::string> session_id; std::optional<std::string> room_id;
                    auto esc = [](const std::string& s){ std::string o; o.reserve(s.size()+8); for (char c: s){ if (c=='"'||c=='\\') o.push_back('\\'); o.push_back(c);} return o; };
                    std::ostringstream js; js << '{'; bool first=true;
                    for (const auto& f : e.fields) {
                        if (f.first == "type") type = f.second;
                        else if (f.first == "ts_ms") ts_ms = f.second;
                        else if (f.first == "user_id") user_id = f.second;
                        else if (f.first == "session_id") session_id = f.second;
                        else if (f.first == "room_id") room_id = f.second;
                        if (!first) js << ','; first=false;
                        js << '"' << esc(f.first) << '"' << ':' << '"' << esc(f.second) << '"';
                    }
                    js << '}';
                    if (type.empty()) type = "unknown";
                    if (ts_ms.empty()) ts_ms = "0";
                    std::string uidv = user_id ? *user_id : std::string();
                    std::string sidv = session_id ? *session_id : std::string();
                    std::string ridv = room_id ? *room_id : std::string();
                    // UUID 형식이 아니면 빈 문자열로 정규화(NULL 저장)
                    if (!uidv.empty() && !is_uuid(uidv)) uidv.clear();
                    if (!sidv.empty() && !is_uuid(sidv)) sidv.clear();
                    if (!ridv.empty() && !is_uuid(ridv)) ridv.clear();
                    long long ts_v = 0; try { ts_v = std::stoll(ts_ms); } catch (...) { ts_v = 0; }
                    w.exec_params(
                        "insert into session_events(event_id, type, ts, user_id, session_id, room_id, payload) "
                        "values ($1, $2, to_timestamp(($3)::bigint/1000.0), nullif($4,'')::uuid, nullif($5,'')::uuid, nullif($6,'')::uuid, $7::jsonb) "
                        "on conflict (event_id) do nothing",
                        e.id, type, ts_v,
                        uidv,
                        sidv,
                        ridv,
                        js.str()
                    );
                    w.commit();
                    (void)redis->xack(stream, group, e.id);
                    ++ok;
                } catch (const std::exception& ex) {
                    ++fail; bool acked = false;
                    if (dlq_on_err && !dlq.empty()) {
                        try {
                            std::vector<std::pair<std::string,std::string>> fields;
                            fields.emplace_back("orig_event_id", e.id);
                            for (const auto& f : e.fields) fields.emplace_back(f.first, f.second);
                            fields.emplace_back("error", ex.what());
                            (void)redis->xadd(dlq, fields, nullptr, std::nullopt, true);
                            (void)redis->xack(stream, group, e.id);
                            acked = true; ++dlqed;
                        } catch (...) {
                            // DLQ 실패: ACK 생략 → 재시도
                        }
                    }
                    if (!acked && ack_on_err) {
                        (void)redis->xack(stream, group, e.id);
                    }
                }
            }
            auto t1 = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            info(std::string("metric=wb_flush wb_commit_ms=") + std::to_string(ms) +
                 " wb_batch_size=" + std::to_string(buf.size()) +
                 " wb_ok_total=" + std::to_string(ok) +
                 " wb_fail_total=" + std::to_string(fail) +
                 " wb_dlq_total=" + std::to_string(dlqed));
            buf.clear(); buf_bytes = 0; last_flush = std::chrono::steady_clock::now();
            return true;
        };

        // XREADGROUP -> batch -> flush 사이클을 반복하며, pending log는 주기적으로 기록한다.
        while (true) {
            std::vector<server::storage::redis::IRedisClient::StreamEntry> entries;
            if (!redis->xreadgroup(stream, group, consumer, 500, static_cast<std::size_t>(batch_max), entries)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (!entries.empty()) {
                for (auto& e : entries) {
                    // Redis에서 받은 StreamEntry를 메모리 버퍼에 모아 두었다가 flush()로 일괄 처리한다.
                    // 간단한 크기 추정: 필드 누계
                    std::size_t est = e.id.size();
                    for (auto& f : e.fields) est += f.first.size() + f.second.size() + 4;
                    buf_bytes += est; buf.emplace_back(std::move(e));
                    if (buf.size() >= batch_max || buf_bytes >= batch_max_bytes) {
                        (void)flush(true);
                    }
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= batch_delay_ms) {
                (void)flush(false);
            }
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pending_log).count() >= 1000) {
                long long pending = 0;
                if (redis->xpending(stream, group, pending)) {
                    // pending 값은 Redis 소비자 그룹 큐에 남은 메시지 개수로, Grafana 경보와 동일하게 맞춘다.
                    server::core::log::info(std::string("metric=wb_pending value=") + std::to_string(pending));
                }
                last_pending_log = now;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "WB worker error: " << e.what() << std::endl;
        return 1;
    }
}
