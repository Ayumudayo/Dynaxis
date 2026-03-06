#include "server/app/config.hpp"
#include "server/core/util/log.hpp"
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>

/**
 * @brief server_app 환경 변수 기반 설정 로더 구현입니다.
 *
 * 실행 인자/환경 변수 우선순위를 명확히 유지해,
 * 로컬 실행과 컨테이너 배포에서 동일한 설정 규칙을 보장합니다.
 */
namespace server::app {

namespace corelog = server::core::log;

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

// 서버 설정을 로드하는 함수
// 우선순위:
// 1. 커맨드 라인 인자 (포트 등)
// 2. 환경 변수 (Docker/K8s 환경에서 주로 사용)
bool ServerConfig::load(int argc, char** argv) {
    // 1. 기본 설정 로드 (커맨드 라인 인자)
    if (argc >= 2) {
        port = static_cast<unsigned short>(std::stoi(argv[1]));
    }

    // 2. 환경 변수 로드 (Docker 또는 OS 환경 변수)
    // .env 파일 로딩 로직은 제거됨. Docker Compose 또는 실행 환경에서 주입된 환경 변수를 사용함.
    if (const char* val = std::getenv("PORT"); val && *val) {
        port = static_cast<unsigned short>(std::stoi(val));
    }

    if (const char* val = std::getenv("LOG_BUFFER_CAPACITY"); val && *val) {
        auto cap = std::strtoull(val, nullptr, 10);
        if (cap > 0) log_buffer_capacity = static_cast<std::size_t>(cap);
    }

    if (const char* val = std::getenv("CHAT_JOB_QUEUE_MAX"); val && *val) {
        auto cap = std::strtoull(val, nullptr, 10);
        if (cap > 0) job_queue_max = static_cast<std::size_t>(cap);
    }
    if (const char* val = std::getenv("CHAT_DB_JOB_QUEUE_MAX"); val && *val) {
        auto cap = std::strtoull(val, nullptr, 10);
        if (cap > 0) db_job_queue_max = static_cast<std::size_t>(cap);
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
    if (const char* val = std::getenv("SERVER_ROLE"); val && *val) {
        server_role = val;
    }
    if (const char* val = std::getenv("SERVER_GAME_MODE"); val && *val) {
        server_game_mode = val;
    }
    if (const char* val = std::getenv("SERVER_REGION"); val && *val) {
        server_region = val;
    }
    if (const char* val = std::getenv("SERVER_SHARD"); val && *val) {
        server_shard = val;
    }
    if (const char* val = std::getenv("SERVER_TAGS"); val && *val) {
        server_tags = split_csv(val);
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
    if (const char* val = std::getenv("SERVER_DRAIN_TIMEOUT_MS"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        if (parsed > 0 && parsed <= 600'000) {
            shutdown_drain_timeout_ms = parsed;
        }
    }
    if (const char* val = std::getenv("SERVER_DRAIN_POLL_MS"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        if (parsed > 0 && parsed <= 5'000) {
            shutdown_drain_poll_ms = parsed;
        }
    }
    if (const char* val = std::getenv("ADMIN_COMMAND_SIGNING_SECRET"); val && *val) {
        admin_command_signing_secret = val;
    }
    if (const char* val = std::getenv("ADMIN_COMMAND_TTL_MS"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        if (parsed > 0) {
            admin_command_ttl_ms = parsed;
        }
    }
    if (const char* val = std::getenv("ADMIN_COMMAND_FUTURE_SKEW_MS"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        admin_command_future_skew_ms = parsed;
    }

    // 6. Lua 스크립팅 설정 (Stream B scaffold)
    if (const char* val = std::getenv("LUA_ENABLED"); val && *val) {
        lua_enabled = (std::strcmp(val, "0") != 0);
    }
    if (const char* val = std::getenv("LUA_SCRIPTS_DIR"); val && *val) {
        lua_scripts_dir = val;
    }
    if (const char* val = std::getenv("LUA_FALLBACK_SCRIPTS_DIR"); val && *val) {
        lua_fallback_scripts_dir = val;
    }
    if (const char* val = std::getenv("LUA_LOCK_PATH"); val && *val) {
        lua_lock_path = val;
    }
    if (const char* val = std::getenv("LUA_RELOAD_INTERVAL_MS"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        if (parsed > 0 && parsed <= 600'000) {
            lua_reload_interval_ms = parsed;
        }
    }
    if (const char* val = std::getenv("LUA_INSTRUCTION_LIMIT"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        if (parsed > 0) {
            lua_instruction_limit = parsed;
        }
    }
    if (const char* val = std::getenv("LUA_MEMORY_LIMIT_BYTES"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        if (parsed > 0) {
            lua_memory_limit_bytes = parsed;
        }
    }
    if (const char* val = std::getenv("LUA_AUTO_DISABLE_THRESHOLD"); val && *val) {
        auto parsed = std::strtoull(val, nullptr, 10);
        if (parsed > 0) {
            lua_auto_disable_threshold = parsed;
        }
    }

    // 7. 메트릭 설정
    if (const char* val = std::getenv("METRICS_PORT"); val && *val) {
        unsigned long v = std::strtoul(val, nullptr, 10);
        if (v > 0 && v < 65536) metrics_port = static_cast<unsigned short>(v);
    }

    return true;
}

} // namespace server::app
