#include "server/app/config.hpp"
#include "server/core/util/log.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <chrono>

namespace server::app {

namespace corelog = server::core::log;

bool ServerConfig::load(int argc, char** argv) {
    // 1. 기본 설정 로드 (커맨드 라인 인자)
    if (argc >= 2) {
        port = static_cast<unsigned short>(std::stoi(argv[1]));
    }

    // 2. 환경 변수 로드 (Docker 또는 OS 환경 변수)
    // .env 파일 로딩 로직은 제거됨. Docker Compose 또는 실행 환경에서 주입된 환경 변수를 사용함.

    if (const char* val = std::getenv("LOG_BUFFER_CAPACITY"); val && *val) {
        auto cap = std::strtoull(val, nullptr, 10);
        if (cap > 0) log_buffer_capacity = static_cast<std::size_t>(cap);
    }

    // 3. 인스턴스 레지스트리 설정
    if (const char* val = std::getenv("SERVER_ADVERTISE_HOST"); val && *val) {
        advertise_host = val;
    }
    advertise_port = port; // 기본값은 리스닝 포트
    if (const char* val = std::getenv("SERVER_ADVERTISE_PORT"); val && *val) {
        try {
            advertise_port = static_cast<unsigned short>(std::stoul(val));
        } catch (...) {
            corelog::warn("Invalid SERVER_ADVERTISE_PORT value; using listen port");
        }
    }
    if (const char* val = std::getenv("SERVER_HEARTBEAT_INTERVAL"); val && *val) {
        try {
            auto parsed = std::stoul(val);
            if (parsed > 0) registry_heartbeat_interval = std::chrono::seconds{static_cast<long long>(parsed)};
        } catch (...) {}
    }
    if (const char* val = std::getenv("SERVER_REGISTRY_PREFIX"); val && *val) {
        registry_prefix = val;
    }
    if (!registry_prefix.empty() && registry_prefix.back() != '/') {
        registry_prefix.push_back('/');
    }
    if (const char* val = std::getenv("SERVER_REGISTRY_TTL"); val && *val) {
        try {
            auto parsed = std::stoul(val);
            if (parsed > 0) registry_ttl = std::chrono::seconds{static_cast<long long>(parsed)};
        } catch (...) {}
    }
    if (const char* val = std::getenv("SERVER_INSTANCE_ID"); val && *val) {
        server_instance_id = val;
    } else {
        server_instance_id = "server-" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // 4. DB 설정
    if (const char* val = std::getenv("DB_URI"); val && *val) db_uri = val;
    if (const char* val = std::getenv("DB_POOL_MIN"); val && *val) db_pool_min = static_cast<std::size_t>(std::strtoul(val, nullptr, 10));
    if (const char* val = std::getenv("DB_POOL_MAX"); val && *val) db_pool_max = static_cast<std::size_t>(std::strtoul(val, nullptr, 10));
    if (const char* val = std::getenv("DB_CONN_TIMEOUT_MS"); val && *val) db_conn_timeout_ms = static_cast<std::uint32_t>(std::strtoul(val, nullptr, 10));
    if (const char* val = std::getenv("DB_QUERY_TIMEOUT_MS"); val && *val) db_query_timeout_ms = static_cast<std::uint32_t>(std::strtoul(val, nullptr, 10));
    if (const char* val = std::getenv("DB_PREPARE_STATEMENTS"); val && *val) db_prepare_statements = (std::strcmp(val, "0") != 0);
    if (const char* val = std::getenv("DB_WORKER_THREADS"); val && *val) db_worker_threads = static_cast<std::size_t>(std::strtoul(val, nullptr, 10));

    // 5. Redis 설정
    if (const char* val = std::getenv("REDIS_URI"); val && *val) redis_uri = val;
    if (const char* val = std::getenv("REDIS_POOL_MAX"); val && *val) redis_pool_max = static_cast<std::size_t>(std::strtoul(val, nullptr, 10));
    if (const char* val = std::getenv("REDIS_USE_STREAMS"); val && *val) redis_use_streams = (std::strcmp(val, "0") != 0);
    if (const char* val = std::getenv("PRESENCE_CLEAN_ON_START"); val && *val) presence_clean_on_start = (std::strcmp(val, "0") != 0);
    if (const char* val = std::getenv("REDIS_CHANNEL_PREFIX"); val && *val) redis_channel_prefix = val;
    if (const char* val = std::getenv("USE_REDIS_PUBSUB"); val && *val) use_redis_pubsub = (std::strcmp(val, "0") != 0);
    if (const char* val = std::getenv("GATEWAY_ID"); val && *val) gateway_id = val;

    // 6. 메트릭 설정
    if (const char* val = std::getenv("METRICS_PORT"); val && *val) {
        unsigned long v = std::strtoul(val, nullptr, 10);
        if (v > 0 && v < 65536) metrics_port = static_cast<unsigned short>(v);
    }

    return true;
}

} // namespace server::app
