#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include "server/core/app/app_host.hpp"
#include "server/core/app/termination_signals.hpp"
#include "server/config/runtime_settings.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/http_server.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/paths.hpp"
#include "server/state/instance_registry.hpp"
#include "server/storage/redis/client.hpp"

namespace corelog = server::core::log;

/**
 * @brief admin_app의 HTTP API/집계/인증 처리 구현입니다.
 *
 * 운영 가시성을 위한 읽기 전용(control-plane) 엔드포인트를 제공해,
 * 서비스 데이터 경로를 건드리지 않고 상태 점검/조사를 수행할 수 있게 합니다.
 */
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::uint16_t kDefaultAdminPort = 39200;
constexpr std::uint16_t kDefaultWorkerMetricsPort = 39093;
constexpr std::uint16_t kDefaultInstanceMetricsPort = 9090;
constexpr std::uint32_t kDefaultPollIntervalMs = 1000;
constexpr std::uint32_t kDefaultTimeoutMs = 1500;
constexpr std::uint32_t kMaxTimeoutMs = 5000;
constexpr std::uint32_t kDefaultLimit = 100;
constexpr std::uint32_t kMaxLimit = 500;
constexpr std::uint32_t kDefaultAuditTrendMaxPoints = 300;
constexpr std::uint32_t kMaxAuditTrendMaxPoints = 5000;
constexpr std::size_t kMaxDisconnectTargets = 200;
constexpr std::size_t kMaxAnnouncementTextBytes = 512;

constexpr std::string_view kAdminUiFileName = "admin_ui.html";

constexpr std::string_view kAdminUiFallbackHtml = R"ADMIN(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Knights Admin Console</title>
  <style>
    body { font-family: "Avenir Next", "Trebuchet MS", sans-serif; padding: 24px; background: #f3f6f7; color: #23323a; }
    .panel { max-width: 700px; margin: 0 auto; padding: 18px; border-radius: 12px; background: #fff; border: 1px solid #dbe3e8; }
    h1 { margin: 0 0 8px; font-size: 22px; }
    p { margin: 8px 0; }
    code { background: #edf3f6; padding: 2px 6px; border-radius: 6px; }
  </style>
</head>
<body>
  <section class="panel">
    <h1>Knights Admin Console</h1>
    <p>UI asset could not be loaded. Check <code>admin_ui.html</code> next to <code>admin_app</code> binary.</p>
    <p>API is still available at <code>/api/v1/overview</code>.</p>
  </section>
</body>
</html>
)ADMIN";

std::string load_admin_ui_html() {
    auto load_text_file = [](const std::filesystem::path& path) -> std::optional<std::string> {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const auto html = buffer.str();
        if (html.empty()) {
            return std::nullopt;
        }
        return html;
    };

    std::vector<std::filesystem::path> candidates;

    try {
        candidates.emplace_back(server::core::util::paths::executable_dir() / std::string(kAdminUiFileName));
    } catch (...) {
    }

    try {
        const auto cwd = std::filesystem::current_path();
        candidates.emplace_back(cwd / std::string(kAdminUiFileName));
        candidates.emplace_back(cwd / "tools" / "admin_app" / std::string(kAdminUiFileName));
    } catch (...) {
    }

    for (const auto& candidate : candidates) {
        if (const auto html = load_text_file(candidate)) {
            corelog::info("admin_app loaded UI asset: " + candidate.string());
            return *html;
        }
    }

    corelog::warn("admin_app UI asset not found; serving fallback HTML");
    return std::string(kAdminUiFallbackHtml);
}

struct HttpUrl {
    std::string host;
    std::uint16_t port{80};
    std::string target{"/"};
};

struct HttpResponse {
    std::uint32_t status_code{0};
    std::string body;
};

struct SplitTarget {
    std::string path;
    std::string query;
};

struct QueryOptions {
    std::uint32_t timeout_ms{kDefaultTimeoutMs};
    std::uint32_t limit{kDefaultLimit};
    std::string cursor;
    bool timeout_overridden{false};
    bool limit_overridden{false};
};

struct QueryParseResult {
    QueryOptions options;
    bool ok{true};
    std::string error_message;
    std::string error_param;
    std::string error_value;
};

enum class AdminAuthMode {
    kOff,
    kHeader,
    kBearer,
    kHeaderOrBearer,
};

struct AuthContext {
    bool ok{true};
    std::string actor{"anonymous"};
    std::string role{"viewer"};
    std::string error_status{"401 Unauthorized"};
    std::string error_code{"UNAUTHORIZED"};
    std::string error_message;
};

std::string read_env_string(const char* key, std::string fallback) {
    if (const char* v = std::getenv(key); v && *v) {
        return std::string(v);
    }
    return fallback;
}

std::uint16_t read_env_u16(const char* key,
                           std::uint16_t fallback,
                           std::uint16_t min_value,
                           std::uint16_t max_value) {
    if (const char* v = std::getenv(key); v && *v) {
        try {
            const auto parsed = std::stoul(v);
            if (parsed >= min_value && parsed <= max_value) {
                return static_cast<std::uint16_t>(parsed);
            }
        } catch (...) {
        }
        corelog::warn(std::string("admin_app invalid ") + key + "; using fallback");
    }
    return fallback;
}

std::uint32_t read_env_u32(const char* key,
                           std::uint32_t fallback,
                           std::uint32_t min_value,
                           std::uint32_t max_value) {
    if (const char* v = std::getenv(key); v && *v) {
        try {
            const auto parsed = std::stoul(v);
            if (parsed >= min_value && parsed <= max_value) {
                return static_cast<std::uint32_t>(parsed);
            }
        } catch (...) {
        }
        corelog::warn(std::string("admin_app invalid ") + key + "; using fallback");
    }
    return fallback;
}

std::uint64_t now_ms() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(now.count());
}

std::string bool_json(bool value) {
    return value ? "true" : "false";
}

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

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

std::string to_lower_ascii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string normalize_role(std::string_view value) {
    return to_lower_ascii(trim_ascii(value));
}

bool is_supported_role(std::string_view role) {
    return role == "viewer" || role == "operator" || role == "admin";
}

int role_rank(std::string_view role) {
    struct RoleRankEntry {
        std::string_view role;
        int rank;
    };

    static constexpr std::array<RoleRankEntry, 2> kRoleRanks{{
        {"admin", 2},
        {"operator", 1},
    }};

    for (const auto& entry : kRoleRanks) {
        if (entry.role == role) {
            return entry.rank;
        }
    }
    return 0;
}

bool has_min_role(std::string_view actual, std::string_view required) {
    return role_rank(actual) >= role_rank(required);
}

std::string url_decode(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    };

    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        if (c == '%' && (i + 2) < raw.size()) {
            const int hi = hex_value(raw[i + 1]);
            const int lo = hex_value(raw[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }

    return out;
}

std::string sanitize_single_line(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\n' || c == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return trim_ascii(out);
}

std::vector<std::string> split_csv_trimmed(std::string_view raw) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= raw.size()) {
        std::size_t end = raw.find(',', start);
        if (end == std::string_view::npos) {
            end = raw.size();
        }

        std::string token = trim_ascii(raw.substr(start, end - start));
        if (!token.empty()) {
            out.push_back(std::move(token));
        }

        if (end == raw.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::string join_csv(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

AdminAuthMode parse_auth_mode(std::string_view raw) {
    const std::string mode = to_lower_ascii(trim_ascii(raw));

    struct AuthModeEntry {
        std::string_view text;
        AdminAuthMode mode;
    };

    static constexpr std::array<AuthModeEntry, 4> kAuthModes{{
        {"header", AdminAuthMode::kHeader},
        {"bearer", AdminAuthMode::kBearer},
        {"header_or_bearer", AdminAuthMode::kHeaderOrBearer},
        {"header-or-bearer", AdminAuthMode::kHeaderOrBearer},
    }};

    for (const auto& entry : kAuthModes) {
        if (entry.text == mode) {
            return entry.mode;
        }
    }

    return AdminAuthMode::kOff;
}

std::string_view auth_mode_to_string(AdminAuthMode mode) {
    switch (mode) {
    case AdminAuthMode::kOff:
        return "off";
    case AdminAuthMode::kHeader:
        return "header";
    case AdminAuthMode::kBearer:
        return "bearer";
    case AdminAuthMode::kHeaderOrBearer:
        return "header_or_bearer";
    }
    return "off";
}

std::optional<std::string> parse_bearer_token(std::string_view authorization_value) {
    constexpr std::string_view kBearerPrefix = "Bearer ";
    if (authorization_value.size() < kBearerPrefix.size()) {
        return std::nullopt;
    }

    const std::string_view prefix = authorization_value.substr(0, kBearerPrefix.size());
    if (to_lower_ascii(prefix) != "bearer ") {
        return std::nullopt;
    }

    const std::string token = trim_ascii(authorization_value.substr(kBearerPrefix.size()));
    if (token.empty()) {
        return std::nullopt;
    }
    return token;
}

std::uint32_t parse_http_status_code(std::string_view status_line) {
    std::istringstream stream{std::string(status_line)};
    std::uint32_t code = 0;
    stream >> code;
    return code;
}

SplitTarget split_target(std::string_view target) {
    SplitTarget out;
    const auto query_pos = target.find('?');
    if (query_pos == std::string_view::npos) {
        out.path = std::string(target);
        return out;
    }
    out.path = std::string(target.substr(0, query_pos));
    out.query = std::string(target.substr(query_pos + 1));
    return out;
}

bool parse_u32_strict(std::string_view raw, std::uint32_t& out_value) {
    if (raw.empty()) {
        return false;
    }
    try {
        std::size_t pos = 0;
        const auto parsed = std::stoull(std::string(raw), &pos, 10);
        if (pos != raw.size() || parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        out_value = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::unordered_map<std::string, std::string> parse_query_string(std::string_view query) {
    std::unordered_map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t end = query.find('&', start);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        const std::string_view pair = query.substr(start, end - start);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out.try_emplace(url_decode(pair), std::string());
            } else {
                out.try_emplace(url_decode(pair.substr(0, eq)), url_decode(pair.substr(eq + 1)));
            }
        }
        if (end == query.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

QueryParseResult parse_common_query_options(std::string_view query) {
    QueryParseResult result;
    if (query.empty()) {
        return result;
    }

    const auto params = parse_query_string(query);

    if (const auto it = params.find("timeout_ms"); it != params.end()) {
        std::uint32_t value = 0;
        if (!parse_u32_strict(it->second, value) || value == 0 || value > kMaxTimeoutMs) {
            result.ok = false;
            result.error_message = "timeout_ms must be between 1 and 5000";
            result.error_param = "timeout_ms";
            result.error_value = it->second;
            return result;
        }
        result.options.timeout_ms = value;
        result.options.timeout_overridden = true;
    }

    if (const auto it = params.find("limit"); it != params.end()) {
        std::uint32_t value = 0;
        if (!parse_u32_strict(it->second, value) || value == 0 || value > kMaxLimit) {
            result.ok = false;
            result.error_message = "limit must be between 1 and 500";
            result.error_param = "limit";
            result.error_value = it->second;
            return result;
        }
        result.options.limit = value;
        result.options.limit_overridden = true;
    }

    if (const auto it = params.find("cursor"); it != params.end()) {
        result.options.cursor = it->second;
    }

    return result;
}

std::string json_details(std::string_view key, std::string_view value) {
    std::ostringstream details;
    details << "{\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"}";
    return details.str();
}

std::string resource_from_path(std::string_view path) {
    struct ExactRoute {
        std::string_view path;
        std::string_view resource;
    };

static constexpr std::array<ExactRoute, 15> kExactRoutes{{
        {"/api/v1/auth/context", "auth_context"},
        {"/api/v1/overview", "overview"},
        {"/api/v1/instances", "instances"},
        {"/api/v1/users", "users"},
        {"/api/v1/users/disconnect", "users_disconnect"},
        {"/api/v1/users/mute", "users_mute"},
        {"/api/v1/users/unmute", "users_unmute"},
        {"/api/v1/users/ban", "users_ban"},
        {"/api/v1/users/unban", "users_unban"},
        {"/api/v1/users/kick", "users_kick"},
        {"/api/v1/announcements", "announcements"},
        {"/api/v1/settings", "runtime_settings"},
        {"/api/v1/worker/write-behind", "worker_write_behind"},
        {"/api/v1/metrics/links", "metrics_links"},
        {"/admin", "admin_ui"},
    }};

    for (const auto& route : kExactRoutes) {
        if (path == route.path) {
            return std::string(route.resource);
        }
    }

    if (path == "/admin/") {
        return "admin_ui";
    }

    struct PrefixRoute {
        std::string_view prefix;
        std::string_view resource;
    };

    static constexpr std::array<PrefixRoute, 2> kPrefixRoutes{{
        {"/api/v1/instances/", "instance_detail"},
        {"/api/v1/sessions/", "session_lookup"},
    }};

    for (const auto& route : kPrefixRoutes) {
        if (path.starts_with(route.prefix)) {
            return std::string(route.resource);
        }
    }

    return "unknown";
}

std::string runtime_setting_range_error_message(const server::config::RuntimeSettingRule& rule) {
    return std::string(rule.key_name) + " must be " +
           std::to_string(rule.min_value) + ".." + std::to_string(rule.max_value);
}

std::string ensure_trailing_slash(std::string value) {
    if (value.empty()) {
        return value;
    }
    if (!value.ends_with('/')) {
        value.push_back('/');
    }
    return value;
}

std::optional<HttpUrl> parse_http_url(std::string_view raw) {
    constexpr std::string_view kPrefix = "http://";
    if (!raw.starts_with(kPrefix)) {
        return std::nullopt;
    }

    std::string_view rest = raw.substr(kPrefix.size());
    if (rest.empty()) {
        return std::nullopt;
    }

    std::string_view host_port = rest;
    std::string_view target = "/";
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        host_port = rest.substr(0, slash);
        target = rest.substr(slash);
    }

    if (host_port.empty()) {
        return std::nullopt;
    }

    std::string host;
    std::uint16_t port = 80;
    if (const auto colon = host_port.rfind(':'); colon != std::string_view::npos) {
        host = std::string(host_port.substr(0, colon));
        const auto port_part = host_port.substr(colon + 1);
        if (host.empty() || port_part.empty()) {
            return std::nullopt;
        }
        try {
            const auto parsed = std::stoul(std::string(port_part));
            if (parsed == 0 || parsed > 65535) {
                return std::nullopt;
            }
            port = static_cast<std::uint16_t>(parsed);
        } catch (...) {
            return std::nullopt;
        }
    } else {
        host = std::string(host_port);
    }

    HttpUrl url;
    url.host = std::move(host);
    url.port = port;
    url.target = target.empty() ? "/" : std::string(target);
    return url;
}

std::optional<HttpResponse> http_get_response(const HttpUrl& url) {
    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(url.host, std::to_string(url.port));

        tcp::socket socket(io);
        asio::connect(socket, endpoints);

        const std::string request =
            "GET " + url.target + " HTTP/1.1\r\n"
            "Host: " + url.host + "\r\n"
            "Connection: close\r\n"
            "Accept: text/plain\r\n\r\n";
        asio::write(socket, asio::buffer(request));

        asio::streambuf response_buf;
        boost::system::error_code ec;
        while (asio::read(socket, response_buf, asio::transfer_at_least(1), ec)) {
        }
        if (ec != asio::error::eof) {
            return std::nullopt;
        }

        const std::string raw(asio::buffers_begin(response_buf.data()), asio::buffers_end(response_buf.data()));
        const auto header_end = raw.find("\r\n\r\n");
        const auto status_end = raw.find("\r\n");
        if (header_end == std::string::npos || status_end == std::string::npos) {
            return std::nullopt;
        }

        const std::string_view status_line(raw.data(), status_end);
        std::istringstream status_stream{std::string(status_line)};
        std::string http_version;
        std::uint32_t status_code = 0;
        status_stream >> http_version >> status_code;
        if (status_code == 0) {
            return std::nullopt;
        }

        HttpResponse http_response;
        http_response.status_code = status_code;
        http_response.body = raw.substr(header_end + 4);
        return http_response;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> http_get_text(const HttpUrl& url) {
    const auto response = http_get_response(url);
    if (!response || response->status_code != 200) {
        return std::nullopt;
    }
    return response->body;
}

std::optional<std::uint64_t> parse_prom_u64(std::string_view metrics_text, std::string_view metric_name) {
    std::istringstream lines{std::string(metrics_text)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (!line.starts_with(metric_name)) {
            continue;
        }
        if (line.size() > metric_name.size()) {
            const char sep = line[metric_name.size()];
            if (sep != ' ' && sep != '\t') {
                continue;
            }
        }

        std::istringstream value_stream(line.substr(metric_name.size()));
        long double value = 0.0L;
        if (!(value_stream >> value)) {
            continue;
        }
        if (value < 0.0L) {
            value = 0.0L;
        }
        return static_cast<std::uint64_t>(value);
    }
    return std::nullopt;
}

class AdminApp {
public:
    int run() {
        server::core::app::install_termination_signal_handlers();

        load_config();
        admin_ui_html_ = load_admin_ui_html();
        init_dependencies();
        init_backends();
        capture_audit_trend_point();

        poller_running_.store(true, std::memory_order_release);
        poller_ = std::thread([this]() { poll_loop(); });

        http_server_ = std::make_unique<server::core::metrics::MetricsHttpServer>(
            metrics_port_,
            [this]() { return render_metrics(); },
            [this]() { return app_host_.healthy() && !app_host_.stop_requested(); },
            [this]() { return app_host_.ready() && app_host_.healthy() && !app_host_.stop_requested(); },
            server::core::metrics::MetricsHttpServer::LogsCallback{},
            [this](bool ok) { return app_host_.health_body(ok); },
            [this](bool ok) { return app_host_.readiness_body(ok); },
            [this](const server::core::metrics::MetricsHttpServer::HttpRequest& request) {
                return handle_route(request);
            });
        http_server_->start();

        corelog::info("admin_app started on METRICS_PORT=" + std::to_string(metrics_port_));

        while (!app_host_.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        app_host_.set_ready(false);
        poller_running_.store(false, std::memory_order_release);
        if (poller_.joinable()) {
            poller_.join();
        }

        if (http_server_) {
            http_server_->stop();
            http_server_.reset();
        }

        corelog::info("admin_app stopped");
        return 0;
    }

private:
    struct WorkerSnapshot {
        bool configured{false};
        bool available{false};
        std::uint64_t updated_at_ms{0};
        std::uint64_t pending{0};
        std::uint64_t flush_total{0};
        std::uint64_t flush_ok_total{0};
        std::uint64_t flush_fail_total{0};
        std::uint64_t flush_dlq_total{0};
        std::uint64_t ack_total{0};
        std::uint64_t ack_fail_total{0};
        std::uint64_t reclaim_total{0};
        std::uint64_t reclaim_error_total{0};
        std::string source_url;
        std::string last_error;
    };

    struct InstanceDetailSnapshot {
        std::string metrics_url;
        std::string ready_reason;
        std::uint64_t updated_at_ms{0};
    };

    struct AuditEvent {
        std::uint64_t request_id{0};
        std::string actor;
        std::string role;
        std::string method;
        std::string path;
        std::string resource;
        std::string result;
        std::uint32_t status_code{0};
        std::uint64_t latency_ms{0};
        std::string source_ip;
    };

    struct AuditTrendPoint {
        std::uint64_t timestamp_ms{0};
        std::uint64_t http_errors_total{0};
        std::uint64_t http_unauthorized_total{0};
        std::uint64_t http_forbidden_total{0};
        std::uint64_t http_server_errors_total{0};
    };

    void load_config() {
        metrics_port_ = read_env_u16("METRICS_PORT", kDefaultAdminPort, 1, 65535);
        poll_interval_ms_ = read_env_u32("ADMIN_POLL_INTERVAL_MS", kDefaultPollIntervalMs, 100, 60000);
        audit_trend_max_points_ = read_env_u32(
            "ADMIN_AUDIT_TREND_MAX_POINTS",
            kDefaultAuditTrendMaxPoints,
            30,
            kMaxAuditTrendMaxPoints);
        instance_metrics_port_ = read_env_u16("ADMIN_INSTANCE_METRICS_PORT", kDefaultInstanceMetricsPort, 1, 65535);

        redis_uri_ = read_env_string("REDIS_URI", "");
        registry_prefix_ = ensure_trailing_slash(read_env_string("SERVER_REGISTRY_PREFIX", "gateway/instances/"));
        session_prefix_ = ensure_trailing_slash(read_env_string("GATEWAY_SESSION_PREFIX", "gateway/session/"));
        redis_channel_prefix_ = read_env_string("REDIS_CHANNEL_PREFIX", "");
        registry_ttl_sec_ = read_env_u32("SERVER_REGISTRY_TTL", 30, 1, 3600);

        worker_metrics_raw_url_ = read_env_string(
            "WB_WORKER_METRICS_URL",
            std::string("http://127.0.0.1:") + std::to_string(kDefaultWorkerMetricsPort) + "/metrics");
        worker_metrics_url_ = parse_http_url(worker_metrics_raw_url_);

        grafana_base_url_ = read_env_string("GRAFANA_BASE_URL", "http://127.0.0.1:33000");
        prometheus_base_url_ = read_env_string("PROMETHEUS_BASE_URL", "http://127.0.0.1:39090");

        auth_mode_raw_ = read_env_string("ADMIN_AUTH_MODE", "off");
        auth_mode_ = parse_auth_mode(auth_mode_raw_);
        auth_off_role_ = normalize_role(read_env_string("ADMIN_OFF_ROLE", "admin"));
        auth_user_header_name_ = to_lower_ascii(read_env_string("ADMIN_AUTH_USER_HEADER", "X-Admin-User"));
        auth_role_header_name_ = to_lower_ascii(read_env_string("ADMIN_AUTH_ROLE_HEADER", "X-Admin-Role"));
        auth_bearer_token_ = read_env_string("ADMIN_BEARER_TOKEN", "");
        auth_bearer_actor_ = read_env_string("ADMIN_BEARER_ACTOR", "token-user");
        auth_bearer_role_ = normalize_role(read_env_string("ADMIN_BEARER_ROLE", "viewer"));

        if (!is_supported_role(auth_bearer_role_)) {
            corelog::warn("admin_app ADMIN_BEARER_ROLE is invalid; fallback to viewer");
            auth_bearer_role_ = "viewer";
        }

        if (!is_supported_role(auth_off_role_)) {
            corelog::warn("admin_app ADMIN_OFF_ROLE is invalid; fallback to admin");
            auth_off_role_ = "admin";
        }

        if ((auth_mode_ == AdminAuthMode::kBearer || auth_mode_ == AdminAuthMode::kHeaderOrBearer)
            && auth_bearer_token_.empty()) {
            corelog::warn("admin_app bearer auth mode selected but ADMIN_BEARER_TOKEN is empty");
        }
    }

    AuthContext authenticate_request(const server::core::metrics::MetricsHttpServer::HttpRequest& request) const {
        auto unauthorized = [&](std::string message) {
            AuthContext ctx;
            ctx.ok = false;
            ctx.actor = "anonymous";
            ctx.role = "none";
            ctx.error_status = "401 Unauthorized";
            ctx.error_code = "UNAUTHORIZED";
            ctx.error_message = std::move(message);
            return ctx;
        };

        auto forbidden = [&](std::string actor, std::string role) {
            AuthContext ctx;
            ctx.ok = false;
            ctx.actor = std::move(actor);
            ctx.role = std::move(role);
            ctx.error_status = "403 Forbidden";
            ctx.error_code = "FORBIDDEN";
            ctx.error_message = "role is not allowed";
            return ctx;
        };

        auto from_header = [&]() -> std::optional<AuthContext> {
            const auto user_it = request.headers.find(auth_user_header_name_);
            if (user_it == request.headers.end()) {
                return std::nullopt;
            }

            const std::string actor = trim_ascii(user_it->second);
            if (actor.empty()) {
                return unauthorized("identity header is empty");
            }

            std::string role = "viewer";
            if (const auto role_it = request.headers.find(auth_role_header_name_); role_it != request.headers.end()) {
                role = normalize_role(role_it->second);
            }
            if (role.empty()) {
                role = "viewer";
            }
            if (!is_supported_role(role)) {
                return forbidden(actor, role);
            }

            AuthContext ctx;
            ctx.ok = true;
            ctx.actor = actor;
            ctx.role = role;
            return ctx;
        };

        auto from_bearer = [&]() -> std::optional<AuthContext> {
            const auto auth_it = request.headers.find("authorization");
            if (auth_it == request.headers.end()) {
                return std::nullopt;
            }

            const auto bearer = parse_bearer_token(auth_it->second);
            if (!bearer) {
                return unauthorized("authorization header must be Bearer token");
            }
            if (auth_bearer_token_.empty()) {
                return unauthorized("bearer auth is not configured");
            }
            if (*bearer != auth_bearer_token_) {
                return unauthorized("invalid bearer token");
            }

            if (!is_supported_role(auth_bearer_role_)) {
                return forbidden(auth_bearer_actor_, auth_bearer_role_);
            }

            AuthContext ctx;
            ctx.ok = true;
            ctx.actor = auth_bearer_actor_;
            ctx.role = auth_bearer_role_;
            return ctx;
        };

        if (auth_mode_ == AdminAuthMode::kOff) {
            AuthContext ctx;
            ctx.ok = true;
            ctx.actor = "anonymous";
            ctx.role = auth_off_role_;
            return ctx;
        }

        if (auth_mode_ == AdminAuthMode::kHeader) {
            if (auto ctx = from_header()) {
                return *ctx;
            }
            return unauthorized("missing identity header");
        }

        if (auth_mode_ == AdminAuthMode::kBearer) {
            if (auto ctx = from_bearer()) {
                return *ctx;
            }
            return unauthorized("missing authorization header");
        }

        if (auto ctx = from_header()) {
            return *ctx;
        }
        if (auto ctx = from_bearer()) {
            return *ctx;
        }
        return unauthorized("missing credentials");
    }

    void emit_audit_log(const AuditEvent& event) const {
        std::ostringstream log;
        log << "{";
        log << "\"event\":\"admin_audit\",";
        log << "\"request_id\":\"admin-" << event.request_id << "\",";
        log << "\"actor\":\"" << json_escape(event.actor) << "\",";
        log << "\"role\":\"" << json_escape(event.role) << "\",";
        log << "\"method\":\"" << json_escape(event.method) << "\",";
        log << "\"path\":\"" << json_escape(event.path) << "\",";
        log << "\"resource\":\"" << json_escape(event.resource) << "\",";
        log << "\"result\":\"" << json_escape(event.result) << "\",";
        log << "\"status_code\":" << event.status_code << ",";
        log << "\"latency_ms\":" << event.latency_ms << ",";
        log << "\"source_ip\":\"" << json_escape(event.source_ip) << "\",";
        log << "\"timestamp\":" << now_ms();
        log << "}";
        corelog::info(log.str());
    }

    void capture_audit_trend_point() {
        AuditTrendPoint point;
        point.timestamp_ms = now_ms();
        point.http_errors_total = http_errors_total_.load(std::memory_order_relaxed);
        point.http_unauthorized_total = http_unauthorized_total_.load(std::memory_order_relaxed);
        point.http_forbidden_total = http_forbidden_total_.load(std::memory_order_relaxed);
        point.http_server_errors_total = http_server_errors_total_.load(std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(cache_mutex_);
        audit_trend_points_.push_back(point);
        const std::size_t max_points = static_cast<std::size_t>(audit_trend_max_points_);
        while (audit_trend_points_.size() > max_points) {
            audit_trend_points_.pop_front();
        }
    }

    std::string make_instance_metrics_url(const server::state::InstanceRecord& item) const {
        return "http://" + item.host + ":" + std::to_string(instance_metrics_port_) + "/metrics";
    }

    std::string make_instance_ready_url(const server::state::InstanceRecord& item) const {
        return "http://" + item.host + ":" + std::to_string(instance_metrics_port_) + "/readyz";
    }

    std::string probe_instance_ready_reason(const server::state::InstanceRecord& item) const {
        const std::string fallback = item.ready ? "ready (registry)" : "not ready (registry)";
        const auto ready_url = parse_http_url(make_instance_ready_url(item));
        if (!ready_url) {
            return fallback;
        }

        const auto response = http_get_response(*ready_url);
        if (!response) {
            return fallback;
        }

        const std::string reason = trim_ascii(response->body);
        if (!reason.empty()) {
            return reason;
        }
        return response->status_code == 200 ? "ready" : "not ready";
    }

    void init_dependencies() {
        app_host_.declare_dependency("admin_api");
        app_host_.declare_dependency("redis", server::core::app::AppHost::DependencyRequirement::kOptional);
        app_host_.declare_dependency("wb_metrics", server::core::app::AppHost::DependencyRequirement::kOptional);

        app_host_.set_dependency_ok("admin_api", true);
        app_host_.set_dependency_ok("redis", false);
        app_host_.set_dependency_ok("wb_metrics", false);

        app_host_.set_healthy(true);
        app_host_.set_ready(true);
    }

    void init_backends() {
        if (!redis_uri_.empty()) {
            server::storage::redis::Options options{};
            options.pool_max = read_env_u32("REDIS_POOL_MAX", 10, 1, 256);
            redis_ = server::storage::redis::make_redis_client(redis_uri_, options);

            if (redis_ && redis_->health_check()) {
                app_host_.set_dependency_ok("redis", true);
                auto adapter = server::state::make_redis_state_client(redis_);
                registry_backend_ = std::make_shared<server::state::RedisInstanceStateBackend>(
                    adapter,
                    registry_prefix_,
                    std::chrono::seconds(registry_ttl_sec_));
                redis_available_.store(true, std::memory_order_relaxed);
            } else {
                redis_available_.store(false, std::memory_order_relaxed);
                app_host_.set_dependency_ok("redis", false);
                corelog::warn("admin_app redis unavailable; instances/session endpoints will return upstream errors");
            }
        } else {
            corelog::warn("admin_app REDIS_URI not set; instances/session endpoints will return upstream errors");
        }

        if (!worker_metrics_url_) {
            corelog::warn("admin_app WB_WORKER_METRICS_URL is invalid; worker endpoint disabled");
        }
    }

    void poll_loop() {
        while (poller_running_.load(std::memory_order_acquire) && !app_host_.stop_requested()) {
            refresh_instances_cache();
            refresh_worker_cache();
            capture_audit_trend_point();
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms_));
        }
    }

    void refresh_instances_cache() {
        if (!registry_backend_) {
            return;
        }

        try {
            auto items = registry_backend_->list_instances();
            std::sort(items.begin(), items.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.instance_id < rhs.instance_id;
            });

            std::unordered_map<std::string, server::state::InstanceRecord> index;
            index.reserve(items.size());
            std::unordered_map<std::string, InstanceDetailSnapshot> details;
            details.reserve(items.size());
            for (const auto& item : items) {
                index[item.instance_id] = item;

                InstanceDetailSnapshot detail;
                detail.metrics_url = make_instance_metrics_url(item);
                detail.ready_reason = probe_instance_ready_reason(item);
                detail.updated_at_ms = now_ms();
                details[item.instance_id] = std::move(detail);
            }

            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                instances_cache_ = std::move(items);
                instances_index_ = std::move(index);
                instance_details_index_ = std::move(details);
                instances_updated_at_ms_ = now_ms();
            }

            redis_available_.store(true, std::memory_order_relaxed);
            app_host_.set_dependency_ok("redis", true);
        } catch (const std::exception& ex) {
            poll_errors_total_.fetch_add(1, std::memory_order_relaxed);
            redis_available_.store(false, std::memory_order_relaxed);
            app_host_.set_dependency_ok("redis", false);
            corelog::warn(std::string("admin_app refresh instances failed: ") + ex.what());
        } catch (...) {
            poll_errors_total_.fetch_add(1, std::memory_order_relaxed);
            redis_available_.store(false, std::memory_order_relaxed);
            app_host_.set_dependency_ok("redis", false);
            corelog::warn("admin_app refresh instances failed");
        }
    }

    void refresh_worker_cache() {
        WorkerSnapshot next;
        next.configured = (worker_metrics_url_.has_value());
        next.source_url = worker_metrics_raw_url_;

        if (!worker_metrics_url_) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            worker_cache_ = std::move(next);
            worker_available_.store(false, std::memory_order_relaxed);
            app_host_.set_dependency_ok("wb_metrics", false);
            return;
        }

        auto text = http_get_text(*worker_metrics_url_);
        if (!text) {
            next.available = false;
            next.last_error = "worker metrics fetch failed";
            std::lock_guard<std::mutex> lock(cache_mutex_);
            worker_cache_ = std::move(next);
            worker_available_.store(false, std::memory_order_relaxed);
            app_host_.set_dependency_ok("wb_metrics", false);
            return;
        }

        next.available = true;
        next.updated_at_ms = now_ms();
        next.pending = parse_prom_u64(*text, "wb_pending").value_or(0);
        next.flush_total = parse_prom_u64(*text, "wb_flush_total").value_or(0);
        next.flush_ok_total = parse_prom_u64(*text, "wb_flush_ok_total").value_or(0);
        next.flush_fail_total = parse_prom_u64(*text, "wb_flush_fail_total").value_or(0);
        next.flush_dlq_total = parse_prom_u64(*text, "wb_flush_dlq_total").value_or(0);
        next.ack_total = parse_prom_u64(*text, "wb_ack_total").value_or(0);
        next.ack_fail_total = parse_prom_u64(*text, "wb_ack_fail_total").value_or(0);
        next.reclaim_total = parse_prom_u64(*text, "wb_reclaim_total").value_or(0);
        next.reclaim_error_total = parse_prom_u64(*text, "wb_reclaim_error_total").value_or(0);

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            worker_cache_ = std::move(next);
        }
        worker_available_.store(true, std::memory_order_relaxed);
        app_host_.set_dependency_ok("wb_metrics", true);
    }

    std::string render_metrics() const {
        std::ostringstream stream;

        server::core::metrics::append_build_info(stream);

        stream << "# TYPE admin_http_requests_total counter\n";
        stream << "admin_http_requests_total " << http_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_http_errors_total counter\n";
        stream << "admin_http_errors_total " << http_errors_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_http_unauthorized_total counter\n";
        stream << "admin_http_unauthorized_total "
               << http_unauthorized_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_http_forbidden_total counter\n";
        stream << "admin_http_forbidden_total "
               << http_forbidden_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_http_server_errors_total counter\n";
        stream << "admin_http_server_errors_total "
               << http_server_errors_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_overview_requests_total counter\n";
        stream << "admin_overview_requests_total " << overview_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_auth_context_requests_total counter\n";
        stream << "admin_auth_context_requests_total "
               << auth_context_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_instances_requests_total counter\n";
        stream << "admin_instances_requests_total " << instances_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_session_lookup_requests_total counter\n";
        stream << "admin_session_lookup_requests_total " << session_lookup_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_users_requests_total counter\n";
        stream << "admin_users_requests_total " << users_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_disconnect_requests_total counter\n";
        stream << "admin_disconnect_requests_total " << disconnect_requests_total_.load(std::memory_order_relaxed)
               << "\n";

        stream << "# TYPE admin_announce_requests_total counter\n";
        stream << "admin_announce_requests_total " << announce_requests_total_.load(std::memory_order_relaxed)
               << "\n";

        stream << "# TYPE admin_settings_requests_total counter\n";
        stream << "admin_settings_requests_total " << settings_requests_total_.load(std::memory_order_relaxed)
               << "\n";

        stream << "# TYPE admin_moderation_requests_total counter\n";
        stream << "admin_moderation_requests_total " << moderation_requests_total_.load(std::memory_order_relaxed)
               << "\n";

        stream << "# TYPE admin_worker_requests_total counter\n";
        stream << "admin_worker_requests_total " << worker_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_poll_errors_total counter\n";
        stream << "admin_poll_errors_total " << poll_errors_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_instances_cached gauge\n";
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            stream << "admin_instances_cached " << instances_cache_.size() << "\n";
        }

        stream << "# TYPE admin_redis_available gauge\n";
        stream << "admin_redis_available " << (redis_available_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

        stream << "# TYPE admin_worker_metrics_available gauge\n";
        stream << "admin_worker_metrics_available " << (worker_available_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

        stream << "# TYPE admin_read_only_mode gauge\n";
        stream << "admin_read_only_mode 0\n";

        stream << app_host_.dependency_metrics_text();
        return stream.str();
    }

    std::optional<server::core::metrics::MetricsHttpServer::RouteResponse>
    handle_route(const server::core::metrics::MetricsHttpServer::HttpRequest& request) {
        const SplitTarget split = split_target(request.target);
        const std::string& path = split.path;
        const bool is_admin_ui = (path == "/admin" || path == "/admin/");
        const bool is_api = path.starts_with("/api/");

        if (!is_admin_ui && !is_api) {
            return std::nullopt;
        }

        const auto started_at = std::chrono::steady_clock::now();
        const std::uint64_t request_id = request_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::string method = request.method.empty() ? std::string("GET") : request.method;
        const std::string resource = resource_from_path(path);

        const AuthContext auth = authenticate_request(request);

        auto finalize = [&](server::core::metrics::MetricsHttpServer::RouteResponse response)
            -> std::optional<server::core::metrics::MetricsHttpServer::RouteResponse> {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at);
            const std::uint32_t status_code = parse_http_status_code(response.status);

            AuditEvent event;
            event.request_id = request_id;
            event.actor = auth.actor;
            event.role = auth.role;
            event.method = method;
            event.path = path;
            event.resource = resource;
            event.result = (status_code >= 200 && status_code < 400) ? "ok" : "error";
            event.status_code = status_code;
            event.latency_ms = static_cast<std::uint64_t>(elapsed.count());
            event.source_ip = request.source_ip.empty() ? std::string("unknown") : request.source_ip;

            switch (status_code) {
            case 401:
                http_unauthorized_total_.fetch_add(1, std::memory_order_relaxed);
                break;
            case 403:
                http_forbidden_total_.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                break;
            }

            if (status_code >= 500) {
                http_server_errors_total_.fetch_add(1, std::memory_order_relaxed);
            }

            emit_audit_log(event);

            return response;
        };

        if (!auth.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(request_id, auth.error_status, auth.error_code, auth.error_message));
        }

        if (is_admin_ui) {
            if (method != "GET") {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return finalize(server::core::metrics::MetricsHttpServer::RouteResponse{
                    "405 Method Not Allowed",
                    "text/plain; charset=utf-8",
                    "Method Not Allowed\n"});
            }
            return finalize(server::core::metrics::MetricsHttpServer::RouteResponse{
                "200 OK",
                "text/html; charset=utf-8",
                admin_ui_html_});
        }

        http_requests_total_.fetch_add(1, std::memory_order_relaxed);

        const bool is_disconnect = (path == "/api/v1/users/disconnect");
        const bool is_announce = (path == "/api/v1/announcements");
        const bool is_settings = (path == "/api/v1/settings");
        const bool is_mute = (path == "/api/v1/users/mute");
        const bool is_unmute = (path == "/api/v1/users/unmute");
        const bool is_ban = (path == "/api/v1/users/ban");
        const bool is_unban = (path == "/api/v1/users/unban");
        const bool is_kick = (path == "/api/v1/users/kick");
        const bool is_user_moderation = is_mute || is_unmute || is_ban || is_unban || is_kick;
        const bool is_write_endpoint = is_disconnect || is_announce || is_settings || is_user_moderation;

        if (!is_write_endpoint && method != "GET") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "this endpoint supports GET only"));
        }

        if (is_disconnect && method != "POST") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "disconnect endpoint requires POST"));
        }

        if (is_announce && method != "POST") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "announcements endpoint requires POST"));
        }

        if (is_settings && method != "PATCH") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "settings endpoint requires PATCH"));
        }

        if (is_user_moderation && method != "POST") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "moderation endpoints require POST"));
        }

        QueryOptions query_options;
        if (!is_write_endpoint) {
            const QueryParseResult query = parse_common_query_options(split.query);
            if (!query.ok) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return finalize(json_error(
                    request_id,
                    "400 Bad Request",
                    "BAD_REQUEST",
                    query.error_message,
                    "{" +
                        std::string("\"parameter\":\"") + json_escape(query.error_param) +
                        "\",\"value\":\"" + json_escape(query.error_value) + "\"}"));
            }
            query_options = query.options;
        }

        auto require_role = [&](std::string_view minimum_role)
            -> std::optional<server::core::metrics::MetricsHttpServer::RouteResponse> {
            if (has_min_role(auth.role, minimum_role)) {
                return std::nullopt;
            }

            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "403 Forbidden",
                "FORBIDDEN",
                "insufficient role for this action",
                "{" + std::string("\"required_role\":\"") + std::string(minimum_role) + "\"}");
        };

        if (path == "/api/v1/auth/context") {
            auth_context_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_auth_context(request_id, auth, query_options));
        }

        if (path == "/api/v1/overview") {
            overview_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_overview(request_id, query_options));
        }

        if (path == "/api/v1/instances") {
            instances_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_instances(request_id, query_options));
        }

        constexpr std::string_view kInstancesPrefix = "/api/v1/instances/";
        if (path.starts_with(kInstancesPrefix)) {
            instances_requests_total_.fetch_add(1, std::memory_order_relaxed);
            const std::string id = std::string(path.substr(kInstancesPrefix.size()));
            return finalize(handle_instance_detail(request_id, id, query_options));
        }

        if (path == "/api/v1/users") {
            users_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_users(request_id, query_options, split.query));
        }

        constexpr std::string_view kSessionsPrefix = "/api/v1/sessions/";
        if (path.starts_with(kSessionsPrefix)) {
            session_lookup_requests_total_.fetch_add(1, std::memory_order_relaxed);
            const std::string client_id = std::string(path.substr(kSessionsPrefix.size()));
            return finalize(handle_session_lookup(request_id, client_id, query_options));
        }

        if (is_disconnect) {
            if (auto forbidden = require_role("admin")) {
                return finalize(std::move(*forbidden));
            }
            disconnect_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_disconnect_users(request_id, auth, split.query));
        }

        if (is_announce) {
            if (auto forbidden = require_role("operator")) {
                return finalize(std::move(*forbidden));
            }
            announce_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_announcement(request_id, auth, split.query));
        }

        if (is_settings) {
            if (auto forbidden = require_role("admin")) {
                return finalize(std::move(*forbidden));
            }
            settings_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_runtime_setting(request_id, auth, split.query));
        }

        if (is_user_moderation) {
            if (auto forbidden = require_role("admin")) {
                return finalize(std::move(*forbidden));
            }
            moderation_requests_total_.fetch_add(1, std::memory_order_relaxed);
            std::string op;
            if (is_mute) {
                op = "mute";
            } else if (is_unmute) {
                op = "unmute";
            } else if (is_ban) {
                op = "ban";
            } else if (is_unban) {
                op = "unban";
            } else {
                op = "kick";
            }
            return finalize(handle_user_moderation(request_id, auth, op, split.query));
        }

        if (path == "/api/v1/worker/write-behind") {
            worker_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_worker(request_id, query_options));
        }

        if (path == "/api/v1/metrics/links") {
            return finalize(handle_metrics_links(request_id, query_options));
        }

        http_errors_total_.fetch_add(1, std::memory_order_relaxed);
        return finalize(json_error(request_id, "404 Not Found", "NOT_FOUND", "endpoint not found"));
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_auth_context(std::uint64_t request_id, const AuthContext& auth, const QueryOptions& options) {
        (void)options;
        std::ostringstream data;
        data << "{";
        data << "\"actor\":\"" << json_escape(auth.actor) << "\",";
        data << "\"role\":\"" << json_escape(auth.role) << "\",";
        data << "\"mode\":\"" << json_escape(auth_mode_to_string(auth_mode_)) << "\",";
        data << "\"headers\":{";
        data << "\"user\":\"" << json_escape(auth_user_header_name_) << "\",";
        data << "\"role\":\"" << json_escape(auth_role_header_name_) << "\"";
        data << "},";
        data << "\"capabilities\":{";
        data << "\"disconnect\":" << bool_json(has_min_role(auth.role, "admin")) << ",";
        data << "\"announce\":" << bool_json(has_min_role(auth.role, "operator")) << ",";
        data << "\"settings\":" << bool_json(has_min_role(auth.role, "admin")) << ",";
        data << "\"moderation\":" << bool_json(has_min_role(auth.role, "admin"));
        data << "}";
        data << "}";
        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_overview(std::uint64_t request_id, const QueryOptions& options) {
        (void)options;
        std::size_t total = 0;
        std::size_t ready = 0;
        std::uint64_t instances_updated_ms = 0;
        WorkerSnapshot worker;
        std::deque<AuditTrendPoint> audit_trend;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            total = instances_cache_.size();
            ready = static_cast<std::size_t>(
                std::count_if(instances_cache_.begin(), instances_cache_.end(), [](const auto& v) { return v.ready; }));
            instances_updated_ms = instances_updated_at_ms_;
            worker = worker_cache_;
            audit_trend = audit_trend_points_;
        }

        std::ostringstream data;
        data << "{";
        data << "\"service\":\"admin_app\",";
        data << "\"mode\":\"control_plane\",";
        data << "\"healthy\":" << bool_json(app_host_.healthy()) << ",";
        data << "\"ready\":" << bool_json(app_host_.ready()) << ",";
        data << "\"stop_requested\":" << bool_json(app_host_.stop_requested()) << ",";
        data << "\"services\":{";
        data << "\"redis\":{";
        data << "\"configured\":" << bool_json(!redis_uri_.empty()) << ",";
        data << "\"available\":" << bool_json(redis_available_.load(std::memory_order_relaxed));
        data << "},";
        data << "\"wb_metrics\":{";
        data << "\"configured\":" << bool_json(worker.configured) << ",";
        data << "\"available\":" << bool_json(worker.available);
        data << "},";
        data << "\"gateway\":{";
        data << "\"configured\":true,";
        data << "\"available\":" << bool_json(redis_available_.load(std::memory_order_relaxed));
        data << "},";
        data << "\"server\":{";
        data << "\"configured\":true,";
        data << "\"available\":" << bool_json(total > 0) << ",";
        data << "\"ready_count\":" << ready << ",";
        data << "\"total_count\":" << total;
        data << "},";
        data << "\"wb_worker\":{";
        data << "\"configured\":" << bool_json(worker.configured) << ",";
        data << "\"available\":" << bool_json(worker.available);
        data << "},";
        data << "\"haproxy\":{";
        data << "\"configured\":false,";
        data << "\"available\":false";
        data << "}";
        data << "},";
        data << "\"counts\":{";
        data << "\"instances_total\":" << total << ",";
        data << "\"instances_ready\":" << ready << ",";
        data << "\"instances_not_ready\":" << (total >= ready ? (total - ready) : 0) << ",";
        data << "\"http_errors_total\":" << http_errors_total_.load(std::memory_order_relaxed) << ",";
        data << "\"http_unauthorized_total\":" << http_unauthorized_total_.load(std::memory_order_relaxed)
             << ",";
        data << "\"http_forbidden_total\":" << http_forbidden_total_.load(std::memory_order_relaxed) << ",";
        data << "\"http_server_errors_total\":" << http_server_errors_total_.load(std::memory_order_relaxed);
        data << "},";
        data << "\"audit_trend\":{";
        data << "\"step_ms\":" << poll_interval_ms_ << ",";
        data << "\"max_points\":" << audit_trend_max_points_ << ",";
        data << "\"points\":[";
        for (std::size_t i = 0; i < audit_trend.size(); ++i) {
            const auto& point = audit_trend[i];
            if (i != 0) {
                data << ",";
            }
            data << "{";
            data << "\"timestamp_ms\":" << point.timestamp_ms << ",";
            data << "\"http_errors_total\":" << point.http_errors_total << ",";
            data << "\"http_unauthorized_total\":" << point.http_unauthorized_total << ",";
            data << "\"http_forbidden_total\":" << point.http_forbidden_total << ",";
            data << "\"http_server_errors_total\":" << point.http_server_errors_total;
            data << "}";
        }
        data << "]";
        data << "},";
        data << "\"updated_at\":{";
        data << "\"instances_ms\":" << instances_updated_ms << ",";
        data << "\"worker_ms\":" << worker.updated_at_ms;
        data << "},";
        data << "\"links\":{";
        data << "\"instances\":\"/api/v1/instances\",";
        data << "\"users\":\"/api/v1/users\",";
        data << "\"worker\":\"/api/v1/worker/write-behind\"";
        data << "}";
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_instances(std::uint64_t request_id, const QueryOptions& options) {
        if (!registry_backend_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis registry unavailable");
        }

        std::vector<server::state::InstanceRecord> items;
        std::uint64_t updated_ms = 0;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            items = instances_cache_;
            updated_ms = instances_updated_at_ms_;
        }

        if (options.timeout_overridden) {
            const std::uint64_t now = now_ms();
            const std::uint64_t age = (updated_ms > 0 && now >= updated_ms) ? (now - updated_ms) : now;
            if (updated_ms == 0 || age > options.timeout_ms) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "504 Gateway Timeout",
                    "TIMEOUT",
                    "instances cache is older than timeout_ms",
                    json_details("timeout_ms", std::to_string(options.timeout_ms)));
            }
        }

        std::size_t cursor_index = 0;
        if (!options.cursor.empty()) {
            std::uint32_t parsed_cursor = 0;
            if (!parse_u32_strict(options.cursor, parsed_cursor)) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "400 Bad Request",
                    "BAD_REQUEST",
                    "cursor must be a non-negative integer",
                    json_details("cursor", options.cursor));
            }
            cursor_index = static_cast<std::size_t>(parsed_cursor);
            if (cursor_index > items.size()) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "400 Bad Request",
                    "BAD_REQUEST",
                    "cursor is out of range",
                    json_details("cursor", options.cursor));
            }
        }

        const std::size_t remaining = items.size() - cursor_index;
        const std::size_t take = std::min<std::size_t>(remaining, static_cast<std::size_t>(options.limit));
        const std::size_t end_index = cursor_index + take;

        std::ostringstream data;
        data << "{";
        data << "\"items\":[";
        for (std::size_t i = cursor_index; i < end_index; ++i) {
            const auto& it = items[i];
            if (i != cursor_index) {
                data << ",";
            }
            data << "{";
            data << "\"instance_id\":\"" << json_escape(it.instance_id) << "\",";
            data << "\"host\":\"" << json_escape(it.host) << "\",";
            data << "\"port\":" << it.port << ",";
            data << "\"role\":\"" << json_escape(it.role) << "\",";
            data << "\"ready\":" << bool_json(it.ready) << ",";
            data << "\"active_sessions\":" << it.active_sessions << ",";
            data << "\"last_heartbeat_ms\":" << it.last_heartbeat_ms << ",";
            data << "\"source\":{";
            data << "\"registry_key\":\"" << json_escape(registry_prefix_ + it.instance_id) << "\"";
            data << "}";
            data << "}";
        }
        data << "],";
        data << "\"paging\":{";
        data << "\"limit\":" << options.limit << ",";
        data << "\"cursor\":";
        if (options.cursor.empty()) {
            data << "null";
        } else {
            data << "\"" << json_escape(options.cursor) << "\"";
        }
        data << ",";
        data << "\"next_cursor\":";
        if (end_index < items.size()) {
            data << "\"" << end_index << "\"";
        } else {
            data << "null";
        }
        data << ",";
        data << "\"total\":" << items.size();
        data << "},";
        data << "\"updated_at_ms\":" << updated_ms;
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_instance_detail(std::uint64_t request_id,
                           const std::string& instance_id,
                           const QueryOptions& options) {
        (void)options;
        if (instance_id.empty() || instance_id.find('/') != std::string::npos) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "400 Bad Request", "BAD_REQUEST", "invalid instance id");
        }

        if (!registry_backend_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis registry unavailable");
        }

        std::optional<server::state::InstanceRecord> item;
        std::optional<InstanceDetailSnapshot> detail;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (const auto it = instances_index_.find(instance_id); it != instances_index_.end()) {
                item = it->second;
            }
            if (const auto it = instance_details_index_.find(instance_id); it != instance_details_index_.end()) {
                detail = it->second;
            }
        }

        if (!item) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "404 Not Found", "NOT_FOUND", "instance not found");
        }

        const std::string metrics_url = detail ? detail->metrics_url : make_instance_metrics_url(*item);
        const std::string ready_reason = detail
            ? detail->ready_reason
            : (item->ready ? std::string("ready (registry)") : std::string("not ready (registry)"));

        std::ostringstream data;
        data << "{";
        data << "\"instance_id\":\"" << json_escape(item->instance_id) << "\",";
        data << "\"host\":\"" << json_escape(item->host) << "\",";
        data << "\"port\":" << item->port << ",";
        data << "\"role\":\"" << json_escape(item->role) << "\",";
        data << "\"ready\":" << bool_json(item->ready) << ",";
        data << "\"ready_reason\":\"" << json_escape(ready_reason) << "\",";
        data << "\"active_sessions\":" << item->active_sessions << ",";
        data << "\"last_heartbeat_ms\":" << item->last_heartbeat_ms << ",";
        data << "\"metrics_url\":\"" << json_escape(metrics_url) << "\",";
        data << "\"source\":{";
        data << "\"registry_key\":\"" << json_escape(registry_prefix_ + item->instance_id) << "\"";
        data << "}";
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_session_lookup(std::uint64_t request_id,
                          const std::string& client_id,
                          const QueryOptions& options) {
        (void)options;
        if (client_id.empty() || client_id.find('/') != std::string::npos) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "400 Bad Request", "BAD_REQUEST", "invalid client id");
        }

        if (!redis_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis session directory unavailable");
        }

        const std::string key = session_prefix_ + client_id;
        std::optional<std::string> backend_id;
        try {
            backend_id = redis_->get(key);
        } catch (...) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis session lookup failed");
        }

        std::optional<server::state::InstanceRecord> backend;
        if (backend_id && !backend_id->empty()) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (const auto it = instances_index_.find(*backend_id); it != instances_index_.end()) {
                backend = it->second;
            }
        }

        std::ostringstream data;
        data << "{";
        data << "\"client_id\":\"" << json_escape(client_id) << "\",";
        data << "\"backend_instance_id\":";
        if (backend_id && !backend_id->empty()) {
            data << "\"" << json_escape(*backend_id) << "\"";
        } else {
            data << "null";
        }
        data << ",";
        data << "\"source\":{";
        data << "\"session_key\":\"" << json_escape(key) << "\"";
        data << "},";
        data << "\"backend\":";
        if (backend) {
            data << "{";
            data << "\"instance_id\":\"" << json_escape(backend->instance_id) << "\",";
            data << "\"host\":\"" << json_escape(backend->host) << "\",";
            data << "\"port\":" << backend->port << ",";
            data << "\"role\":\"" << json_escape(backend->role) << "\",";
            data << "\"ready\":" << bool_json(backend->ready) << ",";
            data << "\"active_sessions\":" << backend->active_sessions;
            data << "}";
        } else {
            data << "null";
        }
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_users(std::uint64_t request_id,
                 const QueryOptions& options,
                 std::string_view query_string) {
        if (!redis_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis user directory unavailable");
        }

        const auto params = parse_query_string(query_string);
        std::string room_filter;
        if (const auto it = params.find("room"); it != params.end()) {
            room_filter = trim_ascii(it->second);
        }

        std::vector<std::string> room_keys;
        if (!room_filter.empty()) {
            room_keys.push_back("room:users:" + room_filter);
        } else {
            if (!redis_->scan_keys("room:users:*", room_keys)) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "503 Service Unavailable",
                    "UPSTREAM_UNAVAILABLE",
                    "redis room users scan failed");
            }
            std::sort(room_keys.begin(), room_keys.end());
        }

        std::unordered_map<std::string, std::string> user_to_room;
        user_to_room.reserve(room_keys.size() * 4);

        for (const auto& room_key : room_keys) {
            std::vector<std::string> members;
            if (!redis_->smembers(room_key, members)) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "503 Service Unavailable",
                    "UPSTREAM_UNAVAILABLE",
                    "redis room members lookup failed");
            }

            const std::string room_name = room_key.starts_with("room:users:")
                ? room_key.substr(std::string("room:users:").size())
                : room_key;

            for (const auto& raw_member : members) {
                const std::string member = trim_ascii(raw_member);
                if (member.empty()) {
                    continue;
                }

                if (const auto it = user_to_room.find(member); it == user_to_room.end()) {
                    user_to_room.emplace(member, room_name);
                }
            }
        }

        std::vector<std::string> users;
        users.reserve(user_to_room.size());
        for (const auto& [user, _] : user_to_room) {
            users.push_back(user);
        }
        std::sort(users.begin(), users.end());

        std::size_t cursor_index = 0;
        if (!options.cursor.empty()) {
            std::uint32_t parsed_cursor = 0;
            if (!parse_u32_strict(options.cursor, parsed_cursor)) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "400 Bad Request",
                    "BAD_REQUEST",
                    "cursor must be a non-negative integer",
                    json_details("cursor", options.cursor));
            }
            cursor_index = static_cast<std::size_t>(parsed_cursor);
            if (cursor_index > users.size()) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "400 Bad Request",
                    "BAD_REQUEST",
                    "cursor is out of range",
                    json_details("cursor", options.cursor));
            }
        }

        const std::size_t remaining = users.size() - cursor_index;
        const std::size_t take = std::min<std::size_t>(remaining, static_cast<std::size_t>(options.limit));
        const std::size_t end_index = cursor_index + take;

        std::vector<std::string> session_keys;
        session_keys.reserve(take);
        for (std::size_t i = cursor_index; i < end_index; ++i) {
            session_keys.push_back(session_prefix_ + users[i]);
        }

        std::vector<std::optional<std::string>> backend_ids;
        backend_ids.resize(session_keys.size());
        if (!session_keys.empty()) {
            bool mget_ok = redis_->mget(session_keys, backend_ids);
            if (!mget_ok || backend_ids.size() != session_keys.size()) {
                backend_ids.clear();
                backend_ids.reserve(session_keys.size());
                for (const auto& key : session_keys) {
                    backend_ids.push_back(redis_->get(key));
                }
            }
        }

        std::unordered_map<std::string, server::state::InstanceRecord> instances;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            instances = instances_index_;
        }

        std::ostringstream data;
        data << "{";
        data << "\"items\":[";
        for (std::size_t i = cursor_index; i < end_index; ++i) {
            const std::size_t offset = i - cursor_index;
            const std::string& user = users[i];
            const auto room_it = user_to_room.find(user);
            const std::string room_name = room_it != user_to_room.end() ? room_it->second : std::string();

            std::string backend_id;
            if (offset < backend_ids.size() && backend_ids[offset] && !backend_ids[offset]->empty()) {
                backend_id = *backend_ids[offset];
            }

            if (i != cursor_index) {
                data << ",";
            }
            data << "{";
            data << "\"client_id\":\"" << json_escape(user) << "\",";
            data << "\"room\":\"" << json_escape(room_name) << "\",";
            data << "\"backend_instance_id\":";
            if (!backend_id.empty()) {
                data << "\"" << json_escape(backend_id) << "\"";
            } else {
                data << "null";
            }
            data << ",";
            data << "\"source\":{";
            data << "\"session_key\":\"" << json_escape(session_prefix_ + user) << "\",";
            data << "\"room_key\":\"" << json_escape(std::string("room:users:") + room_name) << "\"";
            data << "},";
            data << "\"backend\":";
            if (!backend_id.empty()) {
                if (const auto backend_it = instances.find(backend_id); backend_it != instances.end()) {
                    const auto& backend = backend_it->second;
                    data << "{";
                    data << "\"instance_id\":\"" << json_escape(backend.instance_id) << "\",";
                    data << "\"host\":\"" << json_escape(backend.host) << "\",";
                    data << "\"port\":" << backend.port << ",";
                    data << "\"ready\":" << bool_json(backend.ready) << ",";
                    data << "\"active_sessions\":" << backend.active_sessions;
                    data << "}";
                } else {
                    data << "null";
                }
            } else {
                data << "null";
            }
            data << "}";
        }
        data << "],";
        data << "\"paging\":{";
        data << "\"limit\":" << options.limit << ",";
        data << "\"cursor\":";
        if (options.cursor.empty()) {
            data << "null";
        } else {
            data << "\"" << json_escape(options.cursor) << "\"";
        }
        data << ",";
        data << "\"next_cursor\":";
        if (end_index < users.size()) {
            data << "\"" << end_index << "\"";
        } else {
            data << "null";
        }
        data << ",";
        data << "\"total\":" << users.size();
        data << "},";
        data << "\"filter\":{";
        data << "\"room\":";
        if (!room_filter.empty()) {
            data << "\"" << json_escape(room_filter) << "\"";
        } else {
            data << "null";
        }
        data << "},";
        data << "\"updated_at_ms\":" << now_ms();
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_disconnect_users(std::uint64_t request_id,
                            const AuthContext& auth,
                            std::string_view query_string) {
        if (!redis_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis command channel unavailable");
        }

        const auto params = parse_query_string(query_string);
        std::vector<std::string> targets;
        if (const auto it = params.find("client_id"); it != params.end()) {
            const std::string one = trim_ascii(it->second);
            if (!one.empty()) {
                targets.push_back(one);
            }
        }
        if (const auto it = params.find("client_ids"); it != params.end()) {
            auto many = split_csv_trimmed(it->second);
            targets.insert(targets.end(), many.begin(), many.end());
        }

        if (targets.empty()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "client_id or client_ids is required");
        }

        std::vector<std::string> deduped;
        deduped.reserve(targets.size());
        std::unordered_set<std::string> seen;
        for (const auto& target : targets) {
            const std::string normalized = trim_ascii(target);
            if (normalized.empty() || normalized.find('/') != std::string::npos || normalized.size() > 128) {
                continue;
            }
            if (seen.insert(normalized).second) {
                deduped.push_back(normalized);
            }
        }

        if (deduped.empty()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "no valid client ids provided");
        }

        if (deduped.size() > kMaxDisconnectTargets) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "too many client ids",
                json_details("max", std::to_string(kMaxDisconnectTargets)));
        }

        std::string reason = "Disconnected by administrator";
        if (const auto it = params.find("reason"); it != params.end()) {
            const std::string sanitized = sanitize_single_line(it->second);
            if (!sanitized.empty()) {
                reason = sanitized;
            }
        }
        if (reason.size() > 200) {
            reason.resize(200);
        }

        const std::string channel = redis_channel_prefix_ + "fanout:admin:disconnect";
        std::ostringstream payload;
        payload << "op=disconnect\n";
        payload << "actor=" << sanitize_single_line(auth.actor) << "\n";
        payload << "request_id=admin-" << request_id << "\n";
        payload << "reason=" << sanitize_single_line(reason) << "\n";
        payload << "client_ids=" << join_csv(deduped);

        const std::string message = "gw=admin_app\n" + payload.str();
        if (!redis_->publish(channel, message)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "failed to publish disconnect command");
        }

        std::ostringstream data;
        data << "{";
        data << "\"accepted\":true,";
        data << "\"channel\":\"" << json_escape(channel) << "\",";
        data << "\"submitted_count\":" << deduped.size() << ",";
        data << "\"reason\":\"" << json_escape(reason) << "\",";
        data << "\"targets\":[";
        for (std::size_t i = 0; i < deduped.size(); ++i) {
            if (i != 0) {
                data << ",";
            }
            data << "\"" << json_escape(deduped[i]) << "\"";
        }
        data << "]";
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_announcement(std::uint64_t request_id,
                        const AuthContext& auth,
                        std::string_view query_string) {
        if (!redis_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis command channel unavailable");
        }

        const auto params = parse_query_string(query_string);
        auto text_it = params.find("text");
        if (text_it == params.end()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "400 Bad Request", "BAD_REQUEST", "text is required");
        }

        std::string text = sanitize_single_line(text_it->second);
        if (text.empty()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "400 Bad Request", "BAD_REQUEST", "text is required");
        }
        if (text.size() > kMaxAnnouncementTextBytes) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "text is too long",
                json_details("max_bytes", std::to_string(kMaxAnnouncementTextBytes)));
        }

        std::string priority = "info";
        if (const auto it = params.find("priority"); it != params.end()) {
            const std::string normalized = to_lower_ascii(trim_ascii(it->second));
            if (normalized == "info" || normalized == "warn" || normalized == "critical") {
                priority = normalized;
            }
        }

        const std::string channel = redis_channel_prefix_ + "fanout:admin:announce";
        std::ostringstream payload;
        payload << "op=announce\n";
        payload << "actor=" << sanitize_single_line(auth.actor) << "\n";
        payload << "request_id=admin-" << request_id << "\n";
        payload << "priority=" << priority << "\n";
        payload << "text=" << text;

        const std::string message = "gw=admin_app\n" + payload.str();
        if (!redis_->publish(channel, message)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "failed to publish announcement command");
        }

        std::ostringstream data;
        data << "{";
        data << "\"accepted\":true,";
        data << "\"channel\":\"" << json_escape(channel) << "\",";
        data << "\"priority\":\"" << json_escape(priority) << "\",";
        data << "\"text\":\"" << json_escape(text) << "\"";
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_runtime_setting(std::uint64_t request_id,
                           const AuthContext& auth,
                           std::string_view query_string) {
        if (!redis_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis command channel unavailable");
        }

        const auto params = parse_query_string(query_string);
        const auto key_it = params.find("key");
        const auto value_it = params.find("value");
        if (key_it == params.end() || value_it == params.end()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "key and value are required");
        }

        const std::string key = to_lower_ascii(trim_ascii(key_it->second));
        const std::string value = trim_ascii(value_it->second);
        if (key.empty() || value.empty()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "key and value are required");
        }

        std::uint32_t parsed_value = 0;
        if (!parse_u32_strict(value, parsed_value)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "value must be an unsigned integer",
                json_details("value", value));
        }

const auto* setting_rule = server::config::find_runtime_setting_rule(key);
        if (setting_rule == nullptr) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "400 Bad Request", "BAD_REQUEST", "unsupported setting key");
        }

        if (parsed_value < setting_rule->min_value || parsed_value > setting_rule->max_value) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                runtime_setting_range_error_message(*setting_rule));
        }

        const std::string channel = redis_channel_prefix_ + "fanout:admin:settings";
        std::ostringstream payload;
        payload << "op=settings\n";
        payload << "actor=" << sanitize_single_line(auth.actor) << "\n";
        payload << "request_id=admin-" << request_id << "\n";
        payload << "key=" << sanitize_single_line(key) << "\n";
        payload << "value=" << parsed_value;

        const std::string message = "gw=admin_app\n" + payload.str();
        if (!redis_->publish(channel, message)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "failed to publish settings command");
        }

        std::ostringstream data;
        data << "{";
        data << "\"accepted\":true,";
        data << "\"channel\":\"" << json_escape(channel) << "\",";
        data << "\"key\":\"" << json_escape(key) << "\",";
        data << "\"value\":" << parsed_value;
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_user_moderation(std::uint64_t request_id,
                           const AuthContext& auth,
                           std::string_view op,
                           std::string_view query_string) {
        if (!redis_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis command channel unavailable");
        }

        const auto params = parse_query_string(query_string);
        std::vector<std::string> targets;
        if (const auto it = params.find("client_id"); it != params.end()) {
            const std::string one = trim_ascii(it->second);
            if (!one.empty()) {
                targets.push_back(one);
            }
        }
        if (const auto it = params.find("client_ids"); it != params.end()) {
            auto many = split_csv_trimmed(it->second);
            targets.insert(targets.end(), many.begin(), many.end());
        }

        if (targets.empty()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "client_id or client_ids is required");
        }

        std::vector<std::string> deduped;
        deduped.reserve(targets.size());
        std::unordered_set<std::string> seen;
        for (const auto& target : targets) {
            const std::string normalized = trim_ascii(target);
            if (normalized.empty() || normalized.find('/') != std::string::npos || normalized.size() > 128) {
                continue;
            }
            if (seen.insert(normalized).second) {
                deduped.push_back(normalized);
            }
        }

        if (deduped.empty()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "no valid client ids provided");
        }

        if (deduped.size() > kMaxDisconnectTargets) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "too many client ids",
                json_details("max", std::to_string(kMaxDisconnectTargets)));
        }

        std::uint32_t duration_sec = 0;
        if (const auto duration_it = params.find("duration_sec"); duration_it != params.end() && !duration_it->second.empty()) {
            if (!parse_u32_strict(trim_ascii(duration_it->second), duration_sec)) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "400 Bad Request",
                    "BAD_REQUEST",
                    "duration_sec must be an unsigned integer",
                    json_details("value", duration_it->second));
            }
        }

        std::string reason;
        if (const auto reason_it = params.find("reason"); reason_it != params.end()) {
            reason = sanitize_single_line(reason_it->second);
            if (reason.size() > 200) {
                reason.resize(200);
            }
        }

        const std::string channel = redis_channel_prefix_ + "fanout:admin:moderation";
        std::ostringstream payload;
        payload << "op=" << sanitize_single_line(op) << "\n";
        payload << "actor=" << sanitize_single_line(auth.actor) << "\n";
        payload << "request_id=admin-" << request_id << "\n";
        payload << "duration_sec=" << duration_sec << "\n";
        payload << "reason=" << reason << "\n";
        payload << "client_ids=" << join_csv(deduped);

        const std::string message = "gw=admin_app\n" + payload.str();
        if (!redis_->publish(channel, message)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "failed to publish moderation command");
        }

        std::ostringstream data;
        data << "{";
        data << "\"accepted\":true,";
        data << "\"channel\":\"" << json_escape(channel) << "\",";
        data << "\"op\":\"" << json_escape(op) << "\",";
        data << "\"duration_sec\":" << duration_sec << ",";
        data << "\"submitted_count\":" << deduped.size() << ",";
        data << "\"targets\":[";
        for (std::size_t i = 0; i < deduped.size(); ++i) {
            if (i != 0) {
                data << ",";
            }
            data << "\"" << json_escape(deduped[i]) << "\"";
        }
        data << "]";
        data << "}";
        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_worker(std::uint64_t request_id, const QueryOptions& options) {
        WorkerSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            snapshot = worker_cache_;
        }

        if (options.timeout_overridden) {
            const std::uint64_t now = now_ms();
            const std::uint64_t age = (snapshot.updated_at_ms > 0 && now >= snapshot.updated_at_ms)
                ? (now - snapshot.updated_at_ms)
                : now;
            if (snapshot.updated_at_ms == 0 || age > options.timeout_ms) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "504 Gateway Timeout",
                    "TIMEOUT",
                    "worker snapshot is older than timeout_ms",
                    json_details("timeout_ms", std::to_string(options.timeout_ms)));
            }
        }

        if (!snapshot.configured) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "worker metrics url is not configured");
        }

        if (!snapshot.available) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            const std::string reason = snapshot.last_error.empty()
                ? std::string("worker metrics unavailable")
                : snapshot.last_error;
            return json_error(request_id, "503 Service Unavailable", "UPSTREAM_UNAVAILABLE", reason);
        }

        std::ostringstream data;
        data << "{";
        data << "\"source_url\":\"" << json_escape(snapshot.source_url) << "\",";
        data << "\"updated_at_ms\":" << snapshot.updated_at_ms << ",";
        data << "\"pending\":" << snapshot.pending << ",";
        data << "\"flush_total\":" << snapshot.flush_total << ",";
        data << "\"flush_ok_total\":" << snapshot.flush_ok_total << ",";
        data << "\"flush_fail_total\":" << snapshot.flush_fail_total << ",";
        data << "\"flush_dlq_total\":" << snapshot.flush_dlq_total << ",";
        data << "\"ack_total\":" << snapshot.ack_total << ",";
        data << "\"ack_fail_total\":" << snapshot.ack_fail_total << ",";
        data << "\"reclaim_total\":" << snapshot.reclaim_total << ",";
        data << "\"reclaim_error_total\":" << snapshot.reclaim_error_total;
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_metrics_links(std::uint64_t request_id, const QueryOptions& options) {
        (void)options;
        std::ostringstream data;
        data << "{";
        data << "\"grafana\":{";
        data << "\"base_url\":\"" << json_escape(grafana_base_url_) << "\",";
        data << "\"dashboards\":[\"/\"]";
        data << "},";
        data << "\"prometheus\":{";
        data << "\"base_url\":\"" << json_escape(prometheus_base_url_) << "\",";
        data << "\"queries\":[\"chat_session_active\",\"wb_pending\"]";
        data << "}";
        data << "}";
        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    json_ok(std::uint64_t request_id, std::string data_json) const {
        std::ostringstream body;
        body << "{";
        body << "\"data\":" << data_json << ",";
        body << "\"meta\":{";
        body << "\"request_id\":\"admin-" << request_id << "\",";
        body << "\"generated_at_ms\":" << now_ms();
        body << "}";
        body << "}";

        return server::core::metrics::MetricsHttpServer::RouteResponse{
            "200 OK",
            "application/json; charset=utf-8",
            body.str()};
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    json_error(std::uint64_t request_id,
               std::string_view status,
               std::string_view code,
               std::string_view message,
               std::string details_json = "{}") const {
        std::ostringstream body;
        body << "{";
        body << "\"error\":{";
        body << "\"code\":\"" << json_escape(code) << "\",";
        body << "\"message\":\"" << json_escape(message) << "\",";
        body << "\"details\":" << details_json;
        body << "},";
        body << "\"meta\":{";
        body << "\"request_id\":\"admin-" << request_id << "\",";
        body << "\"generated_at_ms\":" << now_ms();
        body << "}";
        body << "}";

        return server::core::metrics::MetricsHttpServer::RouteResponse{
            std::string(status),
            "application/json; charset=utf-8",
            body.str()};
    }

    server::core::app::AppHost app_host_{"admin_app"};
    std::unique_ptr<server::core::metrics::MetricsHttpServer> http_server_;

    std::uint16_t metrics_port_{kDefaultAdminPort};
    std::uint16_t instance_metrics_port_{kDefaultInstanceMetricsPort};
    std::uint32_t poll_interval_ms_{kDefaultPollIntervalMs};
    std::uint32_t audit_trend_max_points_{kDefaultAuditTrendMaxPoints};

    std::string redis_uri_;
    std::string registry_prefix_;
    std::string session_prefix_;
    std::string redis_channel_prefix_;
    std::uint32_t registry_ttl_sec_{30};

    std::string worker_metrics_raw_url_;
    std::optional<HttpUrl> worker_metrics_url_;

    std::string grafana_base_url_;
    std::string prometheus_base_url_;
    std::string admin_ui_html_;

    AdminAuthMode auth_mode_{AdminAuthMode::kOff};
    std::string auth_mode_raw_;
    std::string auth_off_role_;
    std::string auth_user_header_name_;
    std::string auth_role_header_name_;
    std::string auth_bearer_token_;
    std::string auth_bearer_actor_;
    std::string auth_bearer_role_;

    std::shared_ptr<server::storage::redis::IRedisClient> redis_;
    std::shared_ptr<server::state::RedisInstanceStateBackend> registry_backend_;

    std::thread poller_;
    std::atomic<bool> poller_running_{false};

    mutable std::mutex cache_mutex_;
    std::vector<server::state::InstanceRecord> instances_cache_;
    std::unordered_map<std::string, server::state::InstanceRecord> instances_index_;
    std::unordered_map<std::string, InstanceDetailSnapshot> instance_details_index_;
    std::uint64_t instances_updated_at_ms_{0};
    WorkerSnapshot worker_cache_;
    std::deque<AuditTrendPoint> audit_trend_points_;

    std::atomic<bool> redis_available_{false};
    std::atomic<bool> worker_available_{false};

    std::atomic<std::uint64_t> http_requests_total_{0};
    std::atomic<std::uint64_t> http_errors_total_{0};
    std::atomic<std::uint64_t> http_unauthorized_total_{0};
    std::atomic<std::uint64_t> http_forbidden_total_{0};
    std::atomic<std::uint64_t> http_server_errors_total_{0};
    std::atomic<std::uint64_t> overview_requests_total_{0};
    std::atomic<std::uint64_t> auth_context_requests_total_{0};
    std::atomic<std::uint64_t> instances_requests_total_{0};
    std::atomic<std::uint64_t> session_lookup_requests_total_{0};
    std::atomic<std::uint64_t> users_requests_total_{0};
    std::atomic<std::uint64_t> disconnect_requests_total_{0};
    std::atomic<std::uint64_t> announce_requests_total_{0};
    std::atomic<std::uint64_t> settings_requests_total_{0};
    std::atomic<std::uint64_t> moderation_requests_total_{0};
    std::atomic<std::uint64_t> worker_requests_total_{0};
    std::atomic<std::uint64_t> poll_errors_total_{0};
    std::atomic<std::uint64_t> request_seq_{0};
};

} // namespace

int main() {
    try {
        AdminApp app;
        return app.run();
    } catch (const std::exception& ex) {
        corelog::error(std::string("admin_app exception: ") + ex.what());
        return 1;
    } catch (...) {
        corelog::error("admin_app unknown exception");
        return 1;
    }
}
