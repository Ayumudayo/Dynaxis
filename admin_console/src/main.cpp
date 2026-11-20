#include "admin/admin_http_server.hpp"
#include "admin/admin_router.hpp"
#include "admin/admin_types.hpp"
#include "admin/chat_ops_backend.hpp"
#include "admin/inventory_poller.hpp"
#include "admin/ops_config_backend.hpp"
#include "admin/service_catalog.hpp"

#include "server/core/util/log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int) {
    g_shutdown_requested.store(true);
}

std::string env_or_default(const char* key, std::string fallback) {
    if (const char* raw = std::getenv(key); raw && *raw) {
        return std::string(raw);
    }
    return fallback;
}

std::string to_lower_copy(std::string_view sv) {
    std::string out(sv);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

unsigned short env_port_or_default(const char* key, unsigned short fallback) {
    if (const char* raw = std::getenv(key); raw && *raw) {
        try {
            const long p = std::stol(raw);
            if (p > 0 && p <= 65535) {
                return static_cast<unsigned short>(p);
            }
        } catch (...) {
        }
    }
    return fallback;
}

std::uint32_t env_u32_or_default(const char* key, std::uint32_t fallback, std::uint32_t min_v, std::uint32_t max_v) {
    if (const char* raw = std::getenv(key); raw && *raw) {
        try {
            const unsigned long v = std::stoul(raw);
            return static_cast<std::uint32_t>(std::clamp<unsigned long>(v, min_v, max_v));
        } catch (...) {
        }
    }
    return fallback;
}

bool env_bool_or_default(const char* key, bool fallback) {
    if (const char* raw = std::getenv(key); raw && *raw) {
        const std::string lower = to_lower_copy(raw);
        if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
            return true;
        }
        if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
            return false;
        }
    }
    return fallback;
}

dynaxis::admin::AdminConfig load_admin_config_from_env() {
    dynaxis::admin::AdminConfig cfg;
    cfg.listen_port = env_port_or_default("ADMIN_LISTEN_PORT", 9200);
    cfg.env_label = env_or_default("ADMIN_ENV_LABEL", "LOCAL");
    cfg.read_only = env_bool_or_default("ADMIN_READ_ONLY", true);

    const std::string auth_mode_raw = to_lower_copy(env_or_default("ADMIN_AUTH_MODE", "token"));
    cfg.auth_mode = (auth_mode_raw == "development") ? dynaxis::admin::AdminAuthMode::DEVELOPMENT
                                                     : dynaxis::admin::AdminAuthMode::TOKEN;

    const std::string dev_role_raw = to_lower_copy(env_or_default("ADMIN_DEV_ROLE", "viewer"));
    if (dev_role_raw == "admin") {
        cfg.dev_default_role = dynaxis::admin::AdminRole::ADMIN;
    } else if (dev_role_raw == "operator") {
        cfg.dev_default_role = dynaxis::admin::AdminRole::OPERATOR;
    } else {
        cfg.dev_default_role = dynaxis::admin::AdminRole::VIEWER;
    }

    cfg.viewer_token = env_or_default("ADMIN_VIEWER_TOKEN", {});
    cfg.operator_token = env_or_default("ADMIN_OPERATOR_TOKEN", {});
    cfg.admin_token = env_or_default("ADMIN_TOKEN", {});

    cfg.web_root = env_or_default("ADMIN_WEB_ROOT", "admin_console/web");
    if (const char* topo = std::getenv("ADMIN_TOPOLOGY_PATH"); topo && *topo) {
        cfg.topology_path = topo;
    }
    cfg.migrations_dir = env_or_default("MIGRATIONS_DIR", "tools/migrations");
    cfg.target_mode = to_lower_copy(env_or_default("ADMIN_TARGET_MODE", "host"));
    cfg.poll_interval_ms = env_u32_or_default("ADMIN_POLL_INTERVAL_MS", 5000, 500, 60000);
    cfg.probe_timeout_ms = env_u32_or_default("ADMIN_PROBE_TIMEOUT_MS", 1500, 100, 10000);
    cfg.stale_ttl_ms = env_u32_or_default("ADMIN_STALE_TTL_MS", 15000, 1000, 120000);
    cfg.operation_verify_timeout_ms = env_u32_or_default("ADMIN_OP_VERIFY_TIMEOUT_MS", 30000, 1000, 180000);
    cfg.observability_enabled = env_bool_or_default("ADMIN_OBSERVABILITY_ENABLED", false);

    cfg.grafana_base_url = env_or_default("ADMIN_GRAFANA_URL", "http://127.0.0.1:33000");
    cfg.prometheus_base_url = env_or_default("ADMIN_PROMETHEUS_URL", "http://127.0.0.1:39090");
    cfg.haproxy_stats_url = env_or_default("ADMIN_HAPROXY_URL", "http://127.0.0.1:8404/");

    cfg.db_uri = env_or_default("DB_URI", {});
    cfg.redis_uri = env_or_default("REDIS_URI", {});
    cfg.redis_channel_prefix = env_or_default("REDIS_CHANNEL_PREFIX", "dynaxis:");
    cfg.wb_stream_key = env_or_default("REDIS_STREAM_KEY", "session_events");
    cfg.dlq_stream_key = env_or_default("WB_DLQ_STREAM", "session_events_dlq");
    cfg.dead_stream_key = env_or_default("WB_DEAD_STREAM", "session_events_dead");
    cfg.dlq_group = env_or_default("WB_GROUP_DLQ", "wb_dlq_group");
    cfg.dlq_retry_max = env_u32_or_default("WB_RETRY_MAX", 5, 1, 100);

    cfg.redis_host_port = env_port_or_default("REDIS_HOST_PORT", 36379);
    cfg.postgres_host_port = env_port_or_default("POSTGRES_HOST_PORT", 35432);
    cfg.haproxy_stats_host_port = env_port_or_default("HAPROXY_STATS_HOST_PORT", 8404);
    cfg.gateway1_metrics_host_port = env_port_or_default("GATEWAY1_METRICS_HOST_PORT", 36001);
    cfg.gateway2_metrics_host_port = env_port_or_default("GATEWAY2_METRICS_HOST_PORT", 36002);
    cfg.wb_worker_metrics_host_port = env_port_or_default("WB_WORKER_METRICS_HOST_PORT", 39093);
    cfg.prometheus_host_port = env_port_or_default("PROMETHEUS_HOST_PORT", 39090);
    cfg.grafana_host_port = env_port_or_default("GRAFANA_HOST_PORT", 33000);
    cfg.redis_exporter_host_port = env_port_or_default("REDIS_EXPORTER_HOST_PORT", 39121);
    cfg.postgres_exporter_host_port = env_port_or_default("POSTGRES_EXPORTER_HOST_PORT", 39187);
    return cfg;
}

} // namespace

/**
 * @brief 웹 관리 콘솔 v1/v2 제어면(`admin_app`) 프로세스 진입점입니다.
 * @param argc 커맨드라인 인자 개수
 * @param argv 커맨드라인 인자 배열 (`--smoke-check` 지원)
 * @return 종료 코드 (`0` 정상)
 */
int main(int argc, char** argv) {
    bool smoke_check = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--smoke-check") {
            smoke_check = true;
        }
    }

    auto config = load_admin_config_from_env();
    std::vector<std::string> warnings;
    auto catalog = dynaxis::admin::build_service_catalog(config, &warnings);
    for (const auto& warn : warnings) {
        server::core::log::warn(warn);
    }

    if (smoke_check) {
        server::core::log::info("admin_app --smoke-check ok (catalog size=" + std::to_string(catalog.size()) + ")");
        return 0;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    dynaxis::admin::InventoryPoller poller(config, std::move(catalog));
    poller.start();

    auto chat_ops = std::make_shared<dynaxis::admin::PostgresRedisChatOpsBackend>(config);
    auto ops_config = std::make_shared<dynaxis::admin::PostgresOpsConfigBackend>(config, poller);
    dynaxis::admin::AdminRouter router(config, poller, chat_ops, ops_config);
    dynaxis::admin::AdminHttpServer server(config.listen_port, [&router](const dynaxis::admin::AdminHttpRequest& req) {
        return router.handle(req);
    });
    server.start();

    server::core::log::info(
        "Dynaxis Web Admin Console v3 listening on port " + std::to_string(server.bound_port()) +
        " (env=" + config.env_label +
        ", auth=" + (config.auth_mode == dynaxis::admin::AdminAuthMode::DEVELOPMENT ? "development" : "token") +
        ", read_only=" + (config.read_only ? "1" : "0") + ")");

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    poller.stop();
    return 0;
}
