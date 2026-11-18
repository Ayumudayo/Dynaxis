#include "admin/admin_router.hpp"

#include "server/core/metrics/build_info.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace dynaxis::admin {
namespace {

std::uint64_t current_unix_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms < 0 ? 0ULL : static_cast<std::uint64_t>(ms);
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.empty() || a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

std::string_view strip_query(std::string_view target) {
    const std::size_t q = target.find('?');
    return (q == std::string_view::npos) ? target : target.substr(0, q);
}

std::optional<std::string> extract_presented_token(const AdminHttpRequest& request) {
    if (const auto it = request.headers.find("x-admin-token"); it != request.headers.end() && !it->second.empty()) {
        return it->second;
    }
    if (const auto it = request.headers.find("authorization"); it != request.headers.end()) {
        std::string_view val(it->second);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) {
            val.remove_prefix(1);
        }
        if (val.size() > 7) {
            std::string prefix(val.substr(0, 7));
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (prefix == "bearer ") {
                std::string_view token = val.substr(7);
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
                    token.remove_prefix(1);
                }
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
                    token.remove_suffix(1);
                }
                if (!token.empty()) {
                    return std::string(token);
                }
            }
        }
    }
    return std::nullopt;
}

nlohmann::json serialize_service(const ServiceObservation& obs,
                                 std::uint64_t now_ms,
                                 std::uint64_t stale_ttl_ms) {
    const ServiceStatus effective = evaluate_effective_status(obs, now_ms, stale_ttl_ms);
    const std::uint64_t observed_age_ms =
        (obs.observed_at_unix_ms > 0 && now_ms >= obs.observed_at_unix_ms)
            ? (now_ms - obs.observed_at_unix_ms)
            : 0ULL;

    nlohmann::json j{
        {"service_id", obs.service_id},
        {"display_name", obs.display_name},
        {"kind", std::string(service_kind_to_string(obs.kind))},
        {"layer", std::string(topology_layer_to_string(obs.layer))},
        {"desired_count", obs.desired_count},
        {"ready_count", effective == ServiceStatus::UNKNOWN ? 0 : obs.ready_count},
        {"status", std::string(service_status_to_string(effective))},
        {"health_ok", obs.health_ok},
        {"ready_ok", obs.ready_ok},
        {"probe_latency_ms", obs.probe_latency_ms},
        {"observed_at_unix_ms", obs.observed_at_unix_ms},
        {"observed_age_ms", observed_age_ms},
        {"detail", effective == ServiceStatus::UNKNOWN && obs.status != ServiceStatus::UNKNOWN ? "stale observation" : obs.detail},
    };
    if (obs.active_sessions >= 0) {
        j["active_sessions"] = obs.active_sessions;
    }
    if (obs.wb_pending >= 0) {
        j["wb_pending"] = obs.wb_pending;
    }
    if (obs.wb_dlq_count >= 0) {
        j["wb_dlq_count"] = obs.wb_dlq_count;
    }
    if (obs.wb_dlq_total > 0 || obs.layer == TopologyLayer::WORKER) {
        j["wb_dlq_total"] = obs.wb_dlq_total;
    }
    if (obs.active_rooms >= 0) {
        j["active_rooms"] = obs.active_rooms;
    }
    if (obs.applied_migrations >= 0 || obs.expected_migrations > 0) {
        j["applied_migrations"] = obs.applied_migrations;
        j["expected_migrations"] = obs.expected_migrations;
    }
    if (!obs.grafana_url.empty()) {
        j["grafana_url"] = obs.grafana_url;
    }
    if (!obs.external_url.empty()) {
        j["external_url"] = obs.external_url;
    }
    return j;
}

struct StaticAssetSpec {
    std::string_view url_path;
    std::string_view rel_file;
    std::string_view content_type;
};

constexpr std::array<StaticAssetSpec, 7> kStaticAssets{{
    {"/", "index.html", "text/html; charset=utf-8"},
    {"/index.html", "index.html", "text/html; charset=utf-8"},
    {"/styles/tokens.css", "styles/tokens.css", "text/css; charset=utf-8"},
    {"/styles/app.css", "styles/app.css", "text/css; charset=utf-8"},
    {"/js/api.js", "js/api.js", "application/javascript; charset=utf-8"},
    {"/js/render.js", "js/render.js", "application/javascript; charset=utf-8"},
    {"/js/app.js", "js/app.js", "application/javascript; charset=utf-8"},
}};

} // namespace

AdminRouter::AdminRouter(AdminConfig config, InventoryPoller& poller)
    : config_(std::move(config)), poller_(poller) {}

AdminAuthDecision AdminRouter::authorize_api_request(const AdminHttpRequest& request) const {
    const auto presented = extract_presented_token(request);
    if (presented.has_value()) {
        if (!config_.admin_token.empty() && constant_time_equal(*presented, config_.admin_token)) {
            return {true, 200, AdminRole::ADMIN, {}, {}};
        }
        if (!config_.operator_token.empty() && constant_time_equal(*presented, config_.operator_token)) {
            return {true, 200, AdminRole::OPERATOR, {}, {}};
        }
        if (!config_.viewer_token.empty() && constant_time_equal(*presented, config_.viewer_token)) {
            return {true, 200, AdminRole::VIEWER, {}, {}};
        }
        if (config_.auth_mode == AdminAuthMode::TOKEN) {
            return {false, 401, AdminRole::VIEWER, "unauthorized", "Invalid or unknown admin bearer token."};
        }
    }

    if (config_.auth_mode == AdminAuthMode::DEVELOPMENT) {
        return {true, 200, AdminRole::VIEWER, {}, {}};
    }

    if (config_.viewer_token.empty() && config_.operator_token.empty() && config_.admin_token.empty()) {
        return {false, 503, AdminRole::VIEWER, "auth_unconfigured", "Admin token auth mode enabled but no admin tokens configured."};
    }

    return {false, 401, AdminRole::VIEWER, "unauthorized", "Missing Bearer or X-Admin-Token header."};
}

std::optional<AdminHttpResponse> AdminRouter::try_serve_static(std::string_view path) const {
    if (path.find("..") != std::string_view::npos || path.find('\\') != std::string_view::npos) {
        return std::nullopt;
    }

    for (const auto& asset : kStaticAssets) {
        if (asset.url_path == path) {
            const auto disk_path = config_.web_root / std::filesystem::path(asset.rel_file);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(disk_path, ec)) {
                return AdminHttpResponse{"404 Not Found", "text/plain; charset=utf-8", "Web asset missing on disk.\n", {}};
            }
            std::ifstream in(disk_path, std::ios::in | std::ios::binary);
            if (!in.is_open()) {
                return AdminHttpResponse{"500 Internal Server Error", "text/plain; charset=utf-8", "Failed to read web asset.\n", {}};
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            AdminHttpResponse resp;
            resp.status = "200 OK";
            resp.content_type = std::string(asset.content_type);
            resp.body = ss.str();
            resp.extra_headers = "Cache-Control: no-cache\r\n";
            return resp;
        }
    }
    return std::nullopt;
}

AdminHttpResponse AdminRouter::make_json(std::string status, std::string body) const {
    AdminHttpResponse resp;
    resp.status = std::move(status);
    resp.content_type = "application/json; charset=utf-8";
    resp.body = std::move(body);
    resp.extra_headers = "Cache-Control: no-store\r\n";
    return resp;
}

AdminHttpResponse AdminRouter::make_error_json(std::string status,
                                               std::string_view code,
                                               std::string_view message) const {
    const nlohmann::json j{
        {"error", std::string(code)},
        {"message", std::string(message)},
    };
    return make_json(std::move(status), j.dump());
}

AdminHttpResponse AdminRouter::handle(const AdminHttpRequest& request) const {
    const std::string_view path = strip_query(request.target);
    const bool is_read_method = (request.method == "GET" || request.method == "HEAD");

    // 1. Operational self-probes for container health/readiness/metrics
    if (path == "/healthz") {
        if (!is_read_method) {
            return make_error_json("405 Method Not Allowed", "method_not_allowed", "Only GET/HEAD allowed.");
        }
        return make_json("200 OK", R"({"status":"ok"})");
    }

    if (path == "/readyz") {
        if (!is_read_method) {
            return make_error_json("405 Method Not Allowed", "method_not_allowed", "Only GET/HEAD allowed.");
        }
        const bool ready = poller_.has_completed_initial_poll();
        return make_json(ready ? "200 OK" : "503 Service Unavailable",
                         ready ? R"({"status":"ready"})" : R"({"status":"not_ready"})");
    }

    if (path == "/metrics") {
        if (!is_read_method) {
            return make_error_json("405 Method Not Allowed", "method_not_allowed", "Only GET/HEAD allowed.");
        }
        const auto snap = poller_.snapshot();
        std::ostringstream oss;
        server::core::metrics::append_build_info(oss);
        oss << "# TYPE admin_services_total gauge\n";
        oss << "admin_services_total " << (snap ? snap->services.size() : 0) << "\n";
        oss << "# TYPE admin_services_ready gauge\n";
        oss << "admin_services_ready " << (snap ? snap->ready_long_running : 0) << "\n";
        oss << "# TYPE admin_last_poll_unix_ms gauge\n";
        oss << "admin_last_poll_unix_ms " << (snap ? snap->captured_at_unix_ms : 0) << "\n";
        oss << "# TYPE admin_wb_dlq_items gauge\n";
        oss << "admin_wb_dlq_items " << (snap && snap->wb_dlq_count >= 0 ? snap->wb_dlq_count : 0) << "\n";
        return AdminHttpResponse{"200 OK", "text/plain; version=0.0.4", oss.str(), {}};
    }

    // 2. JSON Control-Plane API v1
    if (path.rfind("/api/", 0) == 0) {
        const auto auth = authorize_api_request(request);
        if (!auth.authorized) {
            const std::string status_str =
                (auth.http_status == 503) ? "503 Service Unavailable" : "401 Unauthorized";
            AdminHttpResponse resp = make_error_json(status_str, auth.error_code, auth.message);
            if (auth.http_status == 401) {
                resp.extra_headers += "WWW-Authenticate: Bearer\r\n";
            }
            return resp;
        }

        if (!is_read_method) {
            return make_error_json(
                "403 Forbidden",
                "read_only_mode",
                "Mutating operations are disabled in v1 / ADMIN_READ_ONLY mode.");
        }

        const std::uint64_t now_ms = current_unix_ms();
        const auto snap = poller_.snapshot();

        if (path == "/api/v1/meta") {
            const nlohmann::json j{
                {"version", "v1"},
                {"env_label", config_.env_label},
                {"read_only", config_.read_only},
                {"auth_mode", config_.auth_mode == AdminAuthMode::DEVELOPMENT ? "development" : "token"},
                {"actor_role", std::string(admin_role_to_string(auth.role))},
                {"poll_interval_ms", config_.poll_interval_ms},
                {"stale_ttl_ms", config_.stale_ttl_ms},
                {"observability_enabled", config_.observability_enabled},
                {"links",
                 {
                     {"grafana", config_.grafana_base_url},
                     {"prometheus", config_.prometheus_base_url},
                     {"haproxy", config_.haproxy_stats_url},
                 }},
            };
            return make_json("200 OK", j.dump());
        }

        if (path == "/api/v1/services") {
            nlohmann::json services_json = nlohmann::json::array();
            if (snap) {
                for (const auto& obs : snap->services) {
                    services_json.push_back(serialize_service(obs, now_ms, config_.stale_ttl_ms));
                }
            }
            const nlohmann::json j{
                {"captured_at_unix_ms", snap ? snap->captured_at_unix_ms : 0},
                {"now_unix_ms", now_ms},
                {"stale_ttl_ms", config_.stale_ttl_ms},
                {"services", std::move(services_json)},
            };
            return make_json("200 OK", j.dump());
        }

        constexpr std::string_view kServicePrefix = "/api/v1/services/";
        if (path.rfind(kServicePrefix, 0) == 0) {
            const std::string_view id = path.substr(kServicePrefix.size());
            if (!id.empty() && id.find('/') == std::string_view::npos && snap) {
                for (const auto& obs : snap->services) {
                    if (obs.service_id == id) {
                        const nlohmann::json j{
                            {"now_unix_ms", now_ms},
                            {"service", serialize_service(obs, now_ms, config_.stale_ttl_ms)},
                        };
                        return make_json("200 OK", j.dump());
                    }
                }
            }
            return make_error_json("404 Not Found", "service_not_found", "Unknown service_id in catalog.");
        }

        if (path == "/api/v1/overview") {
            int ready_effective = 0;
            int desired_effective = 0;
            nlohmann::json services_json = nlohmann::json::array();
            if (snap) {
                for (const auto& obs : snap->services) {
                    const ServiceStatus eff = evaluate_effective_status(obs, now_ms, config_.stale_ttl_ms);
                    if (obs.kind != ServiceKind::ONE_SHOT) {
                        desired_effective += obs.desired_count;
                        if (eff == ServiceStatus::READY || eff == ServiceStatus::DEGRADED) {
                            ready_effective += obs.ready_count;
                        }
                    }
                    services_json.push_back(serialize_service(obs, now_ms, config_.stale_ttl_ms));
                }
            }

            const std::uint64_t age_ms =
                (snap && snap->captured_at_unix_ms > 0 && now_ms >= snap->captured_at_unix_ms)
                    ? (now_ms - snap->captured_at_unix_ms)
                    : 0ULL;

            const nlohmann::json j{
                {"meta",
                 {
                     {"version", "v1"},
                     {"env_label", config_.env_label},
                     {"read_only", config_.read_only},
                     {"auth_mode", config_.auth_mode == AdminAuthMode::DEVELOPMENT ? "development" : "token"},
                     {"actor_role", std::string(admin_role_to_string(auth.role))},
                     {"poll_interval_ms", config_.poll_interval_ms},
                     {"stale_ttl_ms", config_.stale_ttl_ms},
                     {"observability_enabled", config_.observability_enabled},
                     {"links",
                      {
                          {"grafana", config_.grafana_base_url},
                          {"prometheus", config_.prometheus_base_url},
                          {"haproxy", config_.haproxy_stats_url},
                      }},
                 }},
                {"captured_at_unix_ms", snap ? snap->captured_at_unix_ms : 0},
                {"now_unix_ms", now_ms},
                {"observed_age_ms", age_ms},
                {"kpis",
                 {
                     {"ready_services", ready_effective},
                     {"desired_services", desired_effective},
                     {"gateway_sessions", snap ? snap->gateway_sessions : 0},
                     {"server_sessions", snap ? snap->server_sessions : 0},
                     {"active_rooms", snap ? snap->active_rooms : -1},
                     {"wb_pending", snap ? snap->wb_pending : -1},
                     {"wb_dlq_count", snap ? snap->wb_dlq_count : -1},
                     {"wb_dlq_total", snap ? snap->wb_dlq_total : 0},
                 }},
                {"services", std::move(services_json)},
            };
            return make_json("200 OK", j.dump());
        }

        return make_error_json("404 Not Found", "not_found", "Unknown API v1 endpoint.");
    }

    // 3. Allowlisted Zero-Build Frontend Assets
    if (is_read_method) {
        if (auto asset_resp = try_serve_static(path)) {
            return *asset_resp;
        }
    }

    return make_error_json("404 Not Found", "not_found", "Resource not found.");
}

} // namespace dynaxis::admin
