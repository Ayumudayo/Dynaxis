#include "admin/admin_router.hpp"

#include "server/core/metrics/build_info.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace dynaxis::admin {
namespace {

using json = nlohmann::json;

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

std::string_view extract_query_string(std::string_view target) {
    const std::size_t q = target.find('?');
    return (q == std::string_view::npos) ? std::string_view() : target.substr(q + 1);
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string url_decode(std::string_view input, bool plus_as_space) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '+' && plus_as_space) {
            out.push_back(' ');
        } else if (ch == '%' && i + 2 < input.size()) {
            const int hi = hex_nibble(input[i + 1]);
            const int lo = hex_nibble(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
            out.push_back(ch);
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query_params(std::string_view query) {
    std::unordered_map<std::string, std::string> params;
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = (amp == std::string_view::npos) ? query : query.substr(0, amp);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos) {
            std::string key = url_decode(pair.substr(0, eq), true);
            std::string val = url_decode(pair.substr(eq + 1), true);
            if (!key.empty()) {
                params[std::move(key)] = std::move(val);
            }
        } else if (!pair.empty()) {
            params[url_decode(pair, true)] = "";
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return params;
}

std::string query_str(const std::unordered_map<std::string, std::string>& params, const char* key) {
    const auto it = params.find(key);
    return (it == params.end()) ? std::string() : it->second;
}

bool query_bool(const std::unordered_map<std::string, std::string>& params, const char* key, bool fallback) {
    const auto it = params.find(key);
    if (it == params.end() || it->second.empty()) {
        return fallback;
    }
    const std::string& v = it->second;
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

std::size_t query_size(const std::unordered_map<std::string, std::string>& params,
                       const char* key,
                       std::size_t fallback,
                       std::size_t min_v,
                       std::size_t max_v) {
    const auto it = params.find(key);
    if (it == params.end() || it->second.empty()) {
        return fallback;
    }
    char* end_ptr = nullptr;
    const unsigned long long parsed = std::strtoull(it->second.c_str(), &end_ptr, 10);
    if (end_ptr == nullptr || *end_ptr != '\0') {
        return fallback;
    }
    return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, min_v, max_v));
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

std::string http_status_text(int code) {
    switch (code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 503: return "503 Service Unavailable";
        default: return "500 Internal Server Error";
    }
}

json build_meta_json(const AdminConfig& config, const InventoryPoller& poller, AdminRole role) {
    return json{
        {"version", "v3"},
        {"env_label", config.env_label},
        {"read_only", config.read_only},
        {"auth_mode", config.auth_mode == AdminAuthMode::DEVELOPMENT ? "development" : "token"},
        {"actor_role", std::string(admin_role_to_string(role))},
        {"mutating_allowed", (!config.read_only && role != AdminRole::VIEWER)},
        {"v3_mutating_allowed", (!config.read_only && role == AdminRole::ADMIN)},
        {"poll_interval_ms", poller.poll_interval_ms()},
        {"stale_ttl_ms", poller.stale_ttl_ms()},
        {"observability_enabled", config.observability_enabled},
        {"links",
         {
             {"grafana", config.grafana_base_url},
             {"prometheus", config.prometheus_base_url},
             {"haproxy", config.haproxy_stats_url},
         }},
    };
}

json serialize_service(const ServiceObservation& obs,
                       std::uint64_t now_ms,
                       std::uint64_t stale_ttl_ms) {
    const ServiceStatus effective = evaluate_effective_status(obs, now_ms, stale_ttl_ms);
    const std::uint64_t observed_age_ms =
        (obs.observed_at_unix_ms > 0 && now_ms >= obs.observed_at_unix_ms)
            ? (now_ms - obs.observed_at_unix_ms)
            : 0ULL;

    json j{
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

constexpr std::array<StaticAssetSpec, 9> kStaticAssets{{
    {"/", "index.html", "text/html; charset=utf-8"},
    {"/index.html", "index.html", "text/html; charset=utf-8"},
    {"/styles/tokens.css", "styles/tokens.css", "text/css; charset=utf-8"},
    {"/styles/app.css", "styles/app.css", "text/css; charset=utf-8"},
    {"/js/api.js", "js/api.js", "application/javascript; charset=utf-8"},
    {"/js/render.js", "js/render.js", "application/javascript; charset=utf-8"},
    {"/js/views_v2.js", "js/views_v2.js", "application/javascript; charset=utf-8"},
    {"/js/views_v3.js", "js/views_v3.js", "application/javascript; charset=utf-8"},
    {"/js/app.js", "js/app.js", "application/javascript; charset=utf-8"},
}};

} // namespace

AdminRouter::AdminRouter(AdminConfig config,
                         InventoryPoller& poller,
                         std::shared_ptr<IChatOpsBackend> chat_ops,
                         std::shared_ptr<IOpsConfigBackend> ops_config)
    : config_(std::move(config)),
      poller_(poller),
      chat_ops_(std::move(chat_ops)),
      ops_config_(std::move(ops_config)) {}

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
        return {true, 200, config_.dev_default_role, {}, {}};
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
    const json j{
        {"error", std::string(code)},
        {"message", std::string(message)},
    };
    return make_json(std::move(status), j.dump());
}

AdminHttpResponse AdminRouter::from_chat_op(const ChatOpHttpResult& res) const {
    return make_json(http_status_text(res.http_status), res.json_body);
}

AdminHttpResponse AdminRouter::handle(const AdminHttpRequest& request) const {
    const std::string_view path = strip_query(request.target);
    const auto qparams = parse_query_params(extract_query_string(request.target));
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

    // 2. JSON Control-Plane API (v1 + v2)
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

        const std::uint64_t now_ms = current_unix_ms();
        const auto snap = poller_.snapshot();
        const std::uint32_t effective_stale_ttl_ms = poller_.stale_ttl_ms();

        if (is_read_method) {
            if (path == "/api/v1/meta") {
                return make_json("200 OK", build_meta_json(config_, poller_, auth.role).dump());
            }

            if (path == "/api/v1/services") {
                json services_json = json::array();
                if (snap) {
                    for (const auto& obs : snap->services) {
                        services_json.push_back(serialize_service(obs, now_ms, effective_stale_ttl_ms));
                    }
                }
                const json j{
                    {"captured_at_unix_ms", snap ? snap->captured_at_unix_ms : 0},
                    {"now_unix_ms", now_ms},
                    {"stale_ttl_ms", effective_stale_ttl_ms},
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
                            const json j{
                                {"now_unix_ms", now_ms},
                                {"service", serialize_service(obs, now_ms, effective_stale_ttl_ms)},
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
                json services_json = json::array();
                if (snap) {
                    for (const auto& obs : snap->services) {
                        const ServiceStatus eff = evaluate_effective_status(obs, now_ms, effective_stale_ttl_ms);
                        if (obs.kind != ServiceKind::ONE_SHOT) {
                            desired_effective += obs.desired_count;
                            if (eff == ServiceStatus::READY || eff == ServiceStatus::DEGRADED) {
                                ready_effective += obs.ready_count;
                            }
                        }
                        services_json.push_back(serialize_service(obs, now_ms, effective_stale_ttl_ms));
                    }
                }

                const std::uint64_t age_ms =
                    (snap && snap->captured_at_unix_ms > 0 && now_ms >= snap->captured_at_unix_ms)
                        ? (now_ms - snap->captured_at_unix_ms)
                        : 0ULL;

                const json j{
                    {"meta", build_meta_json(config_, poller_, auth.role)},
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

            auto require_backend = [&]() -> std::optional<AdminHttpResponse> {
                if (!chat_ops_) {
                    return make_error_json("503 Service Unavailable", "backend_unavailable", "Chat operations backend is not configured.");
                }
                return std::nullopt;
            };

            if (path == "/api/v1/chat/rooms") {
                if (auto err = require_backend()) return *err;
                return from_chat_op(chat_ops_->list_rooms(
                    query_str(qparams, "q"),
                    query_bool(qparams, "include_closed", false),
                    query_size(qparams, "limit", 100, 1, 200)));
            }

            constexpr std::string_view kRoomsPrefix = "/api/v1/chat/rooms/";
            if (path.rfind(kRoomsPrefix, 0) == 0) {
                const std::string_view seg = path.substr(kRoomsPrefix.size());
                if (!seg.empty() && seg.find('/') == std::string_view::npos) {
                    if (auto err = require_backend()) return *err;
                    return from_chat_op(chat_ops_->get_room_detail(url_decode(seg, false)));
                }
                return make_error_json("404 Not Found", "not_found", "Unknown chat room endpoint.");
            }

            if (path == "/api/v1/chat/users") {
                if (auto err = require_backend()) return *err;
                return from_chat_op(chat_ops_->list_users(
                    query_str(qparams, "q"),
                    query_size(qparams, "limit", 100, 1, 200)));
            }

            constexpr std::string_view kUsersPrefix = "/api/v1/chat/users/";
            if (path.rfind(kUsersPrefix, 0) == 0) {
                const std::string_view seg = path.substr(kUsersPrefix.size());
                if (!seg.empty() && seg.find('/') == std::string_view::npos) {
                    if (auto err = require_backend()) return *err;
                    return from_chat_op(chat_ops_->get_user_detail(url_decode(seg, false)));
                }
                return make_error_json("404 Not Found", "not_found", "Unknown chat user endpoint.");
            }

            if (path == "/api/v1/chat/sanctions") {
                if (auto err = require_backend()) return *err;
                return from_chat_op(chat_ops_->list_sanctions(
                    query_bool(qparams, "active_only", true),
                    query_str(qparams, "subject"),
                    query_size(qparams, "limit", 100, 1, 200)));
            }

            if (path == "/api/v1/wb/dlq") {
                if (auto err = require_backend()) return *err;
                return from_chat_op(chat_ops_->inspect_dlq(query_size(qparams, "limit", 25, 1, 100)));
            }

            if (path == "/api/v1/audit-logs") {
                if (auto err = require_backend()) return *err;
                return from_chat_op(chat_ops_->list_audit_logs(
                    query_str(qparams, "actor"),
                    query_str(qparams, "action"),
                    query_str(qparams, "target"),
                    query_str(qparams, "status"),
                    query_size(qparams, "limit", 100, 1, 200)));
            }

            auto require_ops_backend = [&]() -> std::optional<AdminHttpResponse> {
                if (!ops_config_) {
                    return make_error_json(
                        "503 Service Unavailable",
                        "backend_unavailable",
                        "Operations and configuration backend is not configured.");
                }
                return std::nullopt;
            };

            if (path == "/api/v1/operations") {
                if (auto err = require_ops_backend()) return *err;
                return from_chat_op(ops_config_->list_operations(
                    query_str(qparams, "status"),
                    query_size(qparams, "limit", 50, 1, 100)));
            }

            constexpr std::string_view kOpsPrefix = "/api/v1/operations/";
            if (path.rfind(kOpsPrefix, 0) == 0) {
                const std::string_view seg = path.substr(kOpsPrefix.size());
                if (!seg.empty() && seg.find('/') == std::string_view::npos) {
                    if (auto err = require_ops_backend()) return *err;
                    return from_chat_op(ops_config_->get_operation(url_decode(seg, false), true));
                }
                return make_error_json("404 Not Found", "not_found", "Unknown operation endpoint.");
            }

            if (path == "/api/v1/config") {
                if (auto err = require_ops_backend()) return *err;
                return from_chat_op(ops_config_->get_config());
            }

            return make_error_json("404 Not Found", "not_found", "Unknown API endpoint.");
        }

        // Mutating API methods (POST / DELETE)
        json body_json = json::object();
        if (!request.body.empty()) {
            try {
                body_json = json::parse(request.body);
                if (!body_json.is_object()) {
                    return make_error_json("400 Bad Request", "invalid_json", "Request body must be a JSON object.");
                }
            } catch (...) {
                return make_error_json("400 Bad Request", "invalid_json", "Malformed JSON request body.");
            }
        }

        std::string header_idem;
        if (const auto it = request.headers.find("idempotency-key"); it != request.headers.end()) {
            header_idem = it->second;
        }
        const std::string idem_key =
            body_json.contains("idempotency_key") && body_json["idempotency_key"].is_string()
                ? body_json["idempotency_key"].get<std::string>()
                : header_idem;
        const std::string reason =
            body_json.contains("reason") && body_json["reason"].is_string()
                ? body_json["reason"].get<std::string>()
                : query_str(qparams, "reason");

        auto record_denied = [&](std::string_view capability,
                                 std::string_view action,
                                 std::string_view target,
                                 std::string_view deny_code) {
            if (chat_ops_) {
                chat_ops_->record_denied_audit(
                    auth.role, capability, action, target, reason, idem_key, deny_code);
            } else if (ops_config_) {
                ops_config_->record_denied_audit(
                    auth.role, capability, action, target, reason, idem_key, deny_code);
            }
        };

        auto guard_mutating = [&](std::string_view capability,
                                  std::string_view action,
                                  std::string_view target) -> std::optional<AdminHttpResponse> {
            if (config_.read_only) {
                record_denied(capability, action, target, "read_only_mode");
                return make_error_json(
                    "403 Forbidden",
                    "read_only_mode",
                    "Mutating operations are disabled while ADMIN_READ_ONLY is enabled.");
            }
            if (auth.role == AdminRole::VIEWER) {
                record_denied(capability, action, target, "insufficient_role");
                return make_error_json(
                    "403 Forbidden",
                    "insufficient_role",
                    "Operator or admin role is required for mutating operations.");
            }
            if (!chat_ops_) {
                return make_error_json(
                    "503 Service Unavailable",
                    "backend_unavailable",
                    "Chat operations backend is not configured.");
            }
            return std::nullopt;
        };

        auto guard_admin_mutating = [&](std::string_view capability,
                                        std::string_view action,
                                        std::string_view target) -> std::optional<AdminHttpResponse> {
            if (config_.read_only) {
                record_denied(capability, action, target, "read_only_mode");
                return make_error_json(
                    "403 Forbidden",
                    "read_only_mode",
                    "Mutating operations are disabled while ADMIN_READ_ONLY is enabled.");
            }
            if (auth.role != AdminRole::ADMIN) {
                record_denied(capability, action, target, "insufficient_role");
                return make_error_json(
                    "403 Forbidden",
                    "insufficient_role",
                    "Admin role is required for v3 lifecycle and configuration operations.");
            }
            if (!ops_config_) {
                return make_error_json(
                    "503 Service Unavailable",
                    "backend_unavailable",
                    "Operations and configuration backend is not configured.");
            }
            return std::nullopt;
        };

        if (request.method == "POST") {
            constexpr std::string_view kRoomsPrefix = "/api/v1/chat/rooms/";
            constexpr std::string_view kCloseSuffix = "/close";
            if (path.rfind(kRoomsPrefix, 0) == 0 &&
                path.size() > kRoomsPrefix.size() + kCloseSuffix.size() &&
                path.compare(path.size() - kCloseSuffix.size(), kCloseSuffix.size(), kCloseSuffix) == 0) {
                const std::string room_raw = url_decode(
                    path.substr(kRoomsPrefix.size(), path.size() - kRoomsPrefix.size() - kCloseSuffix.size()), false);
                if (room_raw.find('/') == std::string::npos) {
                    if (auto denied = guard_mutating("chat.moderation", "close_room", room_raw)) return *denied;
                    return from_chat_op(chat_ops_->close_room(room_raw, reason, idem_key, auth.role));
                }
            }

            constexpr std::string_view kUsersPrefix = "/api/v1/chat/users/";
            constexpr std::string_view kKickSuffix = "/kick";
            if (path.rfind(kUsersPrefix, 0) == 0 &&
                path.size() > kUsersPrefix.size() + kKickSuffix.size() &&
                path.compare(path.size() - kKickSuffix.size(), kKickSuffix.size(), kKickSuffix) == 0) {
                const std::string subject_raw = url_decode(
                    path.substr(kUsersPrefix.size(), path.size() - kUsersPrefix.size() - kKickSuffix.size()), false);
                if (subject_raw.find('/') == std::string::npos) {
                    if (auto denied = guard_mutating("chat.moderation", "kick_user", subject_raw)) return *denied;
                    return from_chat_op(chat_ops_->kick_user(subject_raw, reason, idem_key, auth.role));
                }
            }

            if (path == "/api/v1/chat/sanctions") {
                const std::string subject = body_json.value("subject", "");
                const std::string kind = body_json.value("kind", "");
                std::uint64_t duration_sec = 0;
                if (body_json.contains("duration_sec") && body_json["duration_sec"].is_number_integer()) {
                    const auto signed_dur = body_json["duration_sec"].get<std::int64_t>();
                    if (signed_dur > 0) {
                        duration_sec = static_cast<std::uint64_t>(signed_dur);
                    }
                }
                const bool confirm_permanent = body_json.value("confirm_permanent", false);
                if (auto denied = guard_mutating("chat.moderation", "create_sanction", subject)) return *denied;
                return from_chat_op(chat_ops_->create_sanction(
                    subject, kind, duration_sec, confirm_permanent, reason, idem_key, auth.role));
            }

            if (path == "/api/v1/wb/dlq/replay") {
                std::size_t max_items = 25;
                if (body_json.contains("max_items") && body_json["max_items"].is_number_integer()) {
                    const auto raw_max = body_json["max_items"].get<std::int64_t>();
                    if (raw_max > 0) {
                        max_items = static_cast<std::size_t>(std::clamp<std::int64_t>(raw_max, 1, 100));
                    }
                }
                if (auto denied = guard_mutating("wb.dlq", "replay_dlq", config_.dlq_stream_key)) return *denied;
                return from_chat_op(chat_ops_->replay_dlq(max_items, reason, idem_key, auth.role));
            }

            if (path == "/api/v1/operations") {
                const std::string op_type = body_json.value("operation_type", "service_restart");
                const std::string target_svc = body_json.value("target_service", "");
                const std::string confirm_target = body_json.value("confirmation_target", "");
                if (auto denied = guard_admin_mutating("ops.lifecycle", op_type, target_svc)) return *denied;
                if (op_type != "service_restart") {
                    return make_error_json(
                        "400 Bad Request",
                        "unsupported_operation_type",
                        "Only 'service_restart' is accepted at /api/v1/operations.");
                }
                return from_chat_op(ops_config_->create_restart_operation(
                    target_svc, confirm_target, reason, idem_key, auth.role));
            }

            if (path == "/api/v1/operations/worker/claim") {
                if (auto denied = guard_admin_mutating("ops.worker", "worker_claim", "queue")) return *denied;
                return from_chat_op(ops_config_->claim_next_queued_operation(auth.role));
            }

            constexpr std::string_view kOpsPrefix = "/api/v1/operations/";
            constexpr std::string_view kWorkerResultSuffix = "/worker-result";
            if (path.rfind(kOpsPrefix, 0) == 0 &&
                path.size() > kOpsPrefix.size() + kWorkerResultSuffix.size() &&
                path.compare(path.size() - kWorkerResultSuffix.size(), kWorkerResultSuffix.size(), kWorkerResultSuffix) == 0) {
                const std::string op_id = url_decode(
                    path.substr(kOpsPrefix.size(), path.size() - kOpsPrefix.size() - kWorkerResultSuffix.size()), false);
                if (op_id.find('/') == std::string::npos) {
                    if (auto denied = guard_admin_mutating("ops.worker", "worker_result", op_id)) return *denied;
                    return from_chat_op(ops_config_->report_worker_result(
                        op_id, body_json.value("outcome", ""), body_json.value("summary", ""), auth.role));
                }
            }

            if (path == "/api/v1/config/apply") {
                if (auto denied = guard_admin_mutating("ops.config", "config_apply", "stack-config")) return *denied;
                return from_chat_op(ops_config_->apply_config(request.body, reason, idem_key, auth.role));
            }

            if (path == "/api/v1/config/rollback") {
                std::int64_t target_rev = 0;
                if (body_json.contains("target_revision") && body_json["target_revision"].is_number_integer()) {
                    target_rev = body_json["target_revision"].get<std::int64_t>();
                }
                const std::string confirm_target = body_json.value("confirmation_target", "");
                if (auto denied = guard_admin_mutating("ops.config", "config_rollback", "stack-config")) return *denied;
                return from_chat_op(ops_config_->rollback_config(
                    target_rev, confirm_target, reason, idem_key, auth.role));
            }
        }

        if (request.method == "DELETE") {
            constexpr std::string_view kSanctionsPrefix = "/api/v1/chat/sanctions/";
            if (path.rfind(kSanctionsPrefix, 0) == 0) {
                const std::string sanction_id = url_decode(path.substr(kSanctionsPrefix.size()), false);
                if (!sanction_id.empty() && sanction_id.find('/') == std::string::npos) {
                    if (auto denied = guard_mutating("chat.moderation", "revoke_sanction", sanction_id)) return *denied;
                    return from_chat_op(chat_ops_->revoke_sanction(sanction_id, reason, idem_key, auth.role));
                }
            }
        }

        if (config_.read_only) {
            record_denied("control_plane", request.method, path, "read_only_mode");
            return make_error_json(
                "403 Forbidden",
                "read_only_mode",
                "Mutating operations are disabled while ADMIN_READ_ONLY is enabled.");
        }

        return make_error_json("405 Method Not Allowed", "method_not_allowed", "Method or endpoint does not accept this write operation.");
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
