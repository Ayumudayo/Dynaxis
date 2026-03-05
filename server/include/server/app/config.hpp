#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <vector>

namespace server::app {

/**
 * @brief 서버 설정을 관리하는 구조체입니다.
 * 환경 변수에서 값을 로드하여 저장합니다.
 */
struct ServerConfig {
    // 기본 설정
    unsigned short port = 5000;
    std::size_t log_buffer_capacity = 0;

    // Backpressure 설정
    std::size_t job_queue_max = 8192;
    std::size_t db_job_queue_max = 4096;

    // 인스턴스 레지스트리 설정
    std::string advertise_host = "127.0.0.1";
    unsigned short advertise_port = 5000;
    std::chrono::seconds registry_heartbeat_interval{5};
    std::string registry_prefix = "gateway/instances/";
    std::chrono::seconds registry_ttl{30};
    std::string server_instance_id;
    std::string server_role = "server";
    std::string server_game_mode = "default";
    std::string server_region = "global";
    std::string server_shard = "default";
    std::vector<std::string> server_tags;

    // 데이터베이스 설정
    std::string db_uri;
    std::size_t db_pool_min = 0;
    std::size_t db_pool_max = 0;
    std::uint32_t db_conn_timeout_ms = 0;
    std::uint32_t db_query_timeout_ms = 0;
    bool db_prepare_statements = true;
    std::size_t db_worker_threads = 0;

    // Redis 설정
    std::string redis_uri;
    std::size_t redis_pool_max = 0;
    bool redis_use_streams = true;
    bool presence_clean_on_start = false;
    std::string redis_channel_prefix;
    bool use_redis_pubsub = true;
    std::string gateway_id = "gw-default";

    // Graceful drain (P1-4)
    std::uint64_t shutdown_drain_timeout_ms = 15'000;
    std::uint64_t shutdown_drain_poll_ms = 100;

    // Admin command integrity (P1-7)
    std::string admin_command_signing_secret;
    std::uint64_t admin_command_ttl_ms = 60'000;
    std::uint64_t admin_command_future_skew_ms = 5'000;

    // Lua scripting (Stream B scaffold)
    bool lua_enabled = false;
    std::string lua_scripts_dir;
    std::string lua_fallback_scripts_dir;
    std::string lua_lock_path;
    std::uint64_t lua_reload_interval_ms = 1'000;
    std::uint64_t lua_instruction_limit = 100'000;
    std::uint64_t lua_memory_limit_bytes = 1'048'576;
    std::uint64_t lua_auto_disable_threshold = 3;

    // 메트릭 설정
    unsigned short metrics_port = 0;

    /**
     * @brief 환경 변수로부터 설정을 로드합니다.
     * @param argc 커맨드라인 인자 개수
     * @param argv 커맨드라인 인자 배열
     * @return true 로드 성공, false 실패
     */
    bool load(int argc, char** argv);
};

} // namespace server::app
