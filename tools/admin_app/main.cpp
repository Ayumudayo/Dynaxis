#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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

#include <nlohmann/json.hpp>

#include "redis_client_factory.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/app/termination_signals.hpp"
#include "server/core/config/runtime_settings.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/http_server.hpp"
#include "server/core/state/instance_registry.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/security/admin_command_auth.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/paths.hpp"

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
constexpr std::uint32_t kDefaultExtMaxClockSkewMs = 5000;

constexpr std::string_view kDefaultExtPluginsDir = "server/plugins";
constexpr std::string_view kDefaultExtPluginsFallbackDir = "server/plugins_builtin";
constexpr std::string_view kDefaultExtScriptsDir = "server/scripts";
constexpr std::string_view kDefaultExtScriptsFallbackDir = "server/scripts_builtin";
constexpr std::string_view kDefaultExtScheduleStorePath = "tasks/runtime_ext_deployments_store.json";

constexpr std::string_view kAdminUiFileName = "admin_ui.html";

constexpr std::string_view kAdminUiFallbackHtml = R"ADMIN(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Dynaxis Admin Console</title>
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
    <h1>Dynaxis Admin Console</h1>
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

struct SelectorParseResult {
    server::core::state::InstanceSelector selector;
    bool selector_specified{false};
    bool all_specified{false};
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

bool read_env_bool(const char* key, bool fallback) {
    if (const char* v = std::getenv(key); v && *v) {
        std::string value(v);
        std::size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
            ++begin;
        }
        std::size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
            --end;
        }
        value = value.substr(begin, end - begin);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (value == "1" || value == "true" || value == "yes" || value == "on") {
            return true;
        }
        if (value == "0" || value == "false" || value == "no" || value == "off") {
            return false;
        }
        corelog::warn(std::string("admin_app invalid ") + key + "; using fallback");
    }
    return fallback;
}

// Admin API role matrix single source of truth.
constexpr std::string_view kRoleRequiredDisconnect = "admin";
constexpr std::string_view kRoleRequiredAnnouncement = "operator";
constexpr std::string_view kRoleRequiredSettings = "admin";
constexpr std::string_view kRoleRequiredModeration = "admin";

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

std::string encode_query_string(const std::unordered_map<std::string, std::string>& params) {
    std::vector<std::pair<std::string, std::string>> ordered;
    ordered.reserve(params.size());
    for (const auto& kv : params) {
        ordered.emplace_back(kv.first, kv.second);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });

    std::ostringstream out;
    bool first = true;
    for (const auto& [key, value] : ordered) {
        if (!first) {
            out << '&';
        }
        first = false;
        out << key << '=' << value;
    }
    return out.str();
}

struct WriteParamsResult {
    bool ok{true};
    std::string error_status;
    std::string error_code;
    std::string error_message;
    std::string merged_query;
};

WriteParamsResult merge_write_params_from_request(
    std::string_view query,
    const server::core::metrics::MetricsHttpServer::HttpRequest& request) {
    WriteParamsResult result;
    auto params = parse_query_string(query);

    if (request.body.empty()) {
        result.merged_query = encode_query_string(params);
        return result;
    }

    std::string content_type;
    if (const auto it = request.headers.find("content-type"); it != request.headers.end()) {
        content_type = to_lower_ascii(trim_ascii(it->second));
    }

    const bool is_json = content_type.rfind("application/json", 0) == 0;
    const bool is_form = content_type.rfind("application/x-www-form-urlencoded", 0) == 0;
    if (!is_json && !is_form) {
        result.ok = false;
        result.error_status = "415 Unsupported Media Type";
        result.error_code = "UNSUPPORTED_CONTENT_TYPE";
        result.error_message = "write endpoints support application/json or application/x-www-form-urlencoded";
        return result;
    }

    if (is_form) {
        const auto body_params = parse_query_string(request.body);
        for (const auto& [k, v] : body_params) {
            params[k] = v;
        }
        result.merged_query = encode_query_string(params);
        return result;
    }

    const auto parsed = nlohmann::json::parse(request.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        result.ok = false;
        result.error_status = "400 Bad Request";
        result.error_code = "BAD_REQUEST";
        result.error_message = "malformed JSON body";
        return result;
    }

    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        if (it.value().is_string()) {
            params[it.key()] = it.value().get<std::string>();
            continue;
        }
        if (it.value().is_number_integer()) {
            params[it.key()] = std::to_string(it.value().get<long long>());
            continue;
        }
        if (it.value().is_number_unsigned()) {
            params[it.key()] = std::to_string(it.value().get<unsigned long long>());
            continue;
        }
        if (it.value().is_array() && it.key() == "client_ids") {
            std::vector<std::string> values;
            for (const auto& item : it.value()) {
                if (item.is_string()) {
                    values.push_back(item.get<std::string>());
                } else if (item.is_number_integer()) {
                    values.push_back(std::to_string(item.get<long long>()));
                } else if (item.is_number_unsigned()) {
                    values.push_back(std::to_string(item.get<unsigned long long>()));
                }
            }
            params[it.key()] = join_csv(values);
        }
    }

    result.merged_query = encode_query_string(params);
    return result;
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

bool parse_u64_strict(std::string_view raw, std::uint64_t& out_value) {
    if (raw.empty()) {
        return false;
    }
    try {
        std::size_t pos = 0;
        const auto parsed = std::stoull(std::string(raw), &pos, 10);
        if (pos != raw.size()) {
            return false;
        }
        out_value = static_cast<std::uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_bool_relaxed(std::string_view raw, bool& out_value) {
    const std::string normalized = to_lower_ascii(trim_ascii(raw));
    if (normalized.empty() || normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        out_value = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        out_value = false;
        return true;
    }
    return false;
}

SelectorParseResult parse_instance_selector_query(std::string_view query) {
    SelectorParseResult result;
    if (query.empty()) {
        return result;
    }

    const auto params = parse_query_string(query);
    auto parse_csv_field = [&](const char* key, std::vector<std::string>& out_values) {
        const auto it = params.find(key);
        if (it == params.end()) {
            return;
        }
        result.selector_specified = true;
        out_values = split_csv_trimmed(it->second);
    };

    if (const auto it = params.find("all"); it != params.end()) {
        result.selector_specified = true;
        result.all_specified = true;
        const std::string normalized = to_lower_ascii(trim_ascii(it->second));
        if (normalized.empty() || normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
            result.selector.all = true;
        } else if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
            result.selector.all = false;
        } else {
            result.ok = false;
            result.error_param = "all";
            result.error_value = it->second;
            result.error_message = "all must be a boolean value";
            return result;
        }
    }

    parse_csv_field("server_ids", result.selector.server_ids);
    parse_csv_field("roles", result.selector.roles);
    parse_csv_field("game_modes", result.selector.game_modes);
    parse_csv_field("regions", result.selector.regions);
    parse_csv_field("shards", result.selector.shards);
    parse_csv_field("tags", result.selector.tags);

    return result;
}

void append_selector_command_fields(std::unordered_map<std::string, std::string>& fields,
                                    const SelectorParseResult& selector_parse) {
    if (!selector_parse.selector_specified) {
        return;
    }

    if (selector_parse.all_specified) {
        fields["all"] = selector_parse.selector.all ? "1" : "0";
    }

    const auto append_csv_field = [&](const char* key, const std::vector<std::string>& values) {
        if (!values.empty()) {
            fields[key] = join_csv(values);
        }
    };

    append_csv_field("server_ids", selector_parse.selector.server_ids);
    append_csv_field("roles", selector_parse.selector.roles);
    append_csv_field("game_modes", selector_parse.selector.game_modes);
    append_csv_field("regions", selector_parse.selector.regions);
    append_csv_field("shards", selector_parse.selector.shards);
    append_csv_field("tags", selector_parse.selector.tags);
}

void append_selector_response_json(std::ostringstream& data, const SelectorParseResult& selector_parse) {
    const auto selector_layer = server::core::state::classify_selector_policy_layer(selector_parse.selector);
    data << "\"selector\":{";
    data << "\"applied\":" << bool_json(selector_parse.selector_specified) << ",";
    data << "\"layer\":\"" << server::core::state::selector_policy_layer_name(selector_layer) << "\"";
    data << "}";
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

static constexpr std::array<ExactRoute, 19> kExactRoutes{{
        {"/api/v1/auth/context", "auth_context"},
        {"/api/v1/overview", "overview"},
        {"/api/v1/instances", "instances"},
        {"/api/v1/users", "users"},
        {"/api/v1/ext/inventory", "ext_inventory"},
        {"/api/v1/ext/precheck", "ext_precheck"},
        {"/api/v1/ext/deployments", "ext_deployments"},
        {"/api/v1/ext/schedules", "ext_schedules"},
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

std::string runtime_setting_range_error_message(const server::core::config::RuntimeSettingRule& rule) {
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
        refresh_ext_inventory_cache();
        load_ext_deployments_store();
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

    struct ExtArtifactInventoryItem {
        std::string artifact_id;
        std::string kind;
        std::string name;
        std::string version;
        std::vector<std::string> hook_scope;
        std::string stage;
        std::uint32_t priority{0};
        std::string exclusive_group;
        std::string checksum;
        std::vector<std::string> target_profiles;
        std::vector<std::string> allowed_decisions;
        std::string description;
        std::string owner;
        std::string manifest_path;
        std::string artifact_path;
        std::vector<std::string> issues;
    };

    struct ExtRolloutStrategy {
        std::string type{"all_at_once"};
        std::vector<std::uint32_t> waves{100};
        bool rollback_on_failure{false};
    };

    struct ExtCommandSpec {
        std::string command_id;
        std::string artifact_id;
        server::core::state::InstanceSelector selector;
        bool selector_specified{false};
        std::optional<std::uint64_t> run_at_utc;
        ExtRolloutStrategy rollout;
    };

    struct ExtPrecheckIssue {
        std::string code;
        std::string message;
    };

    struct ExtPrecheckResult {
        bool ok{false};
        std::size_t target_count{0};
        std::vector<ExtPrecheckIssue> issues;
    };

    struct ExtPrecheckRecord {
        ExtCommandSpec command;
        ExtPrecheckResult result;
        std::uint64_t created_at_ms{0};
    };

    struct ExtDeploymentRecord {
        std::string command_id;
        std::string artifact_id;
        std::string actor;
        std::string status{"pending"};
        std::string status_reason;
        server::core::state::InstanceSelector selector;
        bool selector_specified{false};
        std::optional<std::uint64_t> run_at_utc;
        ExtRolloutStrategy rollout;
        std::string kind;
        std::vector<std::string> hook_scope;
        std::string stage;
        std::uint32_t priority{0};
        std::string exclusive_group;
        std::size_t target_count{0};
        std::size_t applied_targets{0};
        std::vector<ExtPrecheckIssue> issues;
        std::uint64_t created_at_ms{0};
        std::uint64_t updated_at_ms{0};
    };

    struct ExtCommandParseResult {
        bool ok{false};
        std::string error_message;
        ExtCommandSpec command;
    };

    static bool is_ext_valid_hook_scope(std::string_view hook) {
        static constexpr std::array<std::string_view, 6> kAllowedHooks{{
            "on_chat_send",
            "on_login",
            "on_join",
            "on_leave",
            "on_session_event",
            "on_admin_command",
        }};
        return std::find(kAllowedHooks.begin(), kAllowedHooks.end(), hook) != kAllowedHooks.end();
    }

    static bool is_ext_valid_stage(std::string_view stage) {
        static constexpr std::array<std::string_view, 5> kAllowedStages{{
            "pre_validate",
            "mutate",
            "authorize",
            "side_effect",
            "observe",
        }};
        return std::find(kAllowedStages.begin(), kAllowedStages.end(), stage) != kAllowedStages.end();
    }

    static bool is_ext_valid_kind(std::string_view kind) {
        return kind == "native_plugin" || kind == "lua_script";
    }

    static bool is_ext_valid_target_profile(std::string_view profile) {
        return profile == "chat" || profile == "world" || profile == "all";
    }

    static bool is_ext_valid_terminal_decision(std::string_view decision) {
        return decision == "block" || decision == "deny" || decision == "handled"
            || decision == "modify" || decision == "pass" || decision == "allow";
    }

    static std::int32_t terminal_decision_priority(std::string_view decision) {
        if (decision == "block" || decision == "deny") {
            return 0;
        }
        if (decision == "handled") {
            return 1;
        }
        if (decision == "modify") {
            return 2;
        }
        return 3;
    }

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
        admin_read_only_ = read_env_bool("ADMIN_READ_ONLY", false);
        admin_command_signing_secret_ = read_env_string("ADMIN_COMMAND_SIGNING_SECRET", "");

        ext_plugins_dir_ = read_env_string("CHAT_HOOK_PLUGINS_DIR", std::string(kDefaultExtPluginsDir));
        ext_plugins_fallback_dir_ = read_env_string(
            "CHAT_HOOK_FALLBACK_PLUGINS_DIR",
            std::string(kDefaultExtPluginsFallbackDir));
        ext_scripts_dir_ = read_env_string("LUA_SCRIPTS_DIR", std::string(kDefaultExtScriptsDir));
        ext_scripts_fallback_dir_ = read_env_string(
            "LUA_FALLBACK_SCRIPTS_DIR",
            std::string(kDefaultExtScriptsFallbackDir));
        ext_schedule_store_path_ = read_env_string(
            "ADMIN_EXT_SCHEDULE_STORE_PATH",
            std::string(kDefaultExtScheduleStorePath));
        ext_max_clock_skew_ms_ = read_env_u32(
            "ADMIN_EXT_MAX_CLOCK_SKEW_MS",
            kDefaultExtMaxClockSkewMs,
            0,
            60000);
        ext_force_fail_wave_index_ = read_env_u32(
            "ADMIN_EXT_FORCE_FAIL_WAVE_INDEX",
            0,
            0,
            32);

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

        if (admin_read_only_) {
            corelog::warn("admin_app write endpoints are disabled by ADMIN_READ_ONLY=1");
        }
        if (admin_command_signing_secret_.empty()) {
            corelog::warn("admin_app ADMIN_COMMAND_SIGNING_SECRET is empty; write command publish will be rejected");
        }
    }

    std::optional<std::string>
    build_signed_admin_message(std::unordered_map<std::string, std::string> fields, std::uint64_t request_id) {
        if (admin_command_signing_secret_.empty()) {
            command_signing_errors_total_.fetch_add(1, std::memory_order_relaxed);
            corelog::warn("admin_app command publish blocked: ADMIN_COMMAND_SIGNING_SECRET is not configured");
            return std::nullopt;
        }

        if (!server::core::security::admin_command_auth::append_signature_fields(
                fields,
                admin_command_signing_secret_,
                server::core::security::admin_command_auth::now_ms())) {
            command_signing_errors_total_.fetch_add(1, std::memory_order_relaxed);
            corelog::error("admin_app command signing failed request_id=admin-" + std::to_string(request_id));
            return std::nullopt;
        }

        return std::string("gw=admin_app\n")
             + server::core::security::admin_command_auth::to_kv_payload(fields);
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

    std::string make_instance_metrics_url(const server::core::state::InstanceRecord& item) const {
        return "http://" + item.host + ":" + std::to_string(instance_metrics_port_) + "/metrics";
    }

    std::string make_instance_ready_url(const server::core::state::InstanceRecord& item) const {
        return "http://" + item.host + ":" + std::to_string(instance_metrics_port_) + "/readyz";
    }

    std::string probe_instance_ready_reason(const server::core::state::InstanceRecord& item) const {
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
            server::core::storage::redis::Options options{};
            options.pool_max = read_env_u32("REDIS_POOL_MAX", 10, 1, 256);
            redis_ = admin_app::make_redis_client(redis_uri_, options);

            if (redis_ && redis_->health_check()) {
                app_host_.set_dependency_ok("redis", true);
                registry_backend_ = admin_app::make_registry_backend(
                    redis_,
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
            refresh_ext_inventory_cache();
            process_ext_deployment_queue();
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

            std::unordered_map<std::string, server::core::state::InstanceRecord> index;
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

    std::filesystem::path resolve_ext_path(std::string_view raw_path) const {
        std::filesystem::path path = std::filesystem::path(std::string(raw_path));
        if (!path.is_absolute()) {
            path = std::filesystem::current_path() / path;
        }
        return path.lexically_normal();
    }

    std::vector<std::filesystem::path> scan_ext_manifest_paths_from_dir(const std::filesystem::path& root) const {
        std::vector<std::filesystem::path> manifests;
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || ec) {
            return manifests;
        }

        std::filesystem::recursive_directory_iterator it(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file()) {
                continue;
            }
            const auto name = it->path().filename().string();
            if (name.ends_with(".plugin.json") || name.ends_with(".script.json")) {
                manifests.push_back(it->path());
            }
        }
        return manifests;
    }

    std::filesystem::path derive_ext_artifact_path(const std::filesystem::path& manifest_path,
                                                   std::string_view kind) const {
        const std::string filename = manifest_path.filename().string();
        if (kind == "native_plugin" && filename.ends_with(".plugin.json")) {
            const std::string stem = filename.substr(0, filename.size() - std::string(".plugin.json").size());
            static constexpr std::array<std::string_view, 4> kExts{{".so", ".dll", ".dylib", ".cpp"}};
            for (const auto& ext : kExts) {
                const auto candidate = manifest_path.parent_path() / (stem + std::string(ext));
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec) && !ec) {
                    return candidate;
                }
            }
            return manifest_path.parent_path() / (stem + ".so");
        }
        if (kind == "lua_script" && filename.ends_with(".script.json")) {
            const std::string stem = filename.substr(0, filename.size() - std::string(".script.json").size());
            return manifest_path.parent_path() / (stem + ".lua");
        }
        return {};
    }

    static std::vector<std::string> parse_json_string_list(const nlohmann::json& node) {
        std::vector<std::string> out;
        if (!node.is_array()) {
            return out;
        }
        for (const auto& item : node) {
            if (!item.is_string()) {
                continue;
            }
            std::string value = trim_ascii(item.get<std::string>());
            if (!value.empty()) {
                out.push_back(std::move(value));
            }
        }
        return out;
    }

    static std::string ext_status_text(std::string_view status) {
        if (status == "pending") {
            return "pending";
        }
        if (status == "precheck_passed") {
            return "precheck_passed";
        }
        if (status == "executing") {
            return "executing";
        }
        if (status == "completed") {
            return "completed";
        }
        if (status == "failed") {
            return "failed";
        }
        if (status == "cancelled") {
            return "cancelled";
        }
        return "failed";
    }

    std::string make_ext_artifact_id(std::string_view kind, const std::filesystem::path& manifest_path) const {
        const std::string filename = manifest_path.filename().string();
        std::string stem = filename;
        if (kind == "native_plugin" && filename.ends_with(".plugin.json")) {
            stem = filename.substr(0, filename.size() - std::string(".plugin.json").size());
            return "plugin:" + stem;
        }
        if (kind == "lua_script" && filename.ends_with(".script.json")) {
            stem = filename.substr(0, filename.size() - std::string(".script.json").size());
            return "script:" + stem;
        }
        return "artifact:" + stem;
    }

    ExtArtifactInventoryItem parse_ext_manifest_file(const std::filesystem::path& manifest_path) const {
        ExtArtifactInventoryItem item;
        item.manifest_path = manifest_path.string();

        std::string expected_kind;
        const std::string filename = manifest_path.filename().string();
        if (filename.ends_with(".plugin.json")) {
            expected_kind = "native_plugin";
        } else if (filename.ends_with(".script.json")) {
            expected_kind = "lua_script";
        }

        std::ifstream in(manifest_path, std::ios::binary);
        if (!in) {
            item.issues.push_back("manifest_read_failed");
            item.kind = expected_kind;
            item.artifact_id = make_ext_artifact_id(item.kind.empty() ? "native_plugin" : item.kind, manifest_path);
            return item;
        }

        std::ostringstream buf;
        buf << in.rdbuf();
        const auto parsed = nlohmann::json::parse(buf.str(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            item.issues.push_back("manifest_json_invalid");
            item.kind = expected_kind;
            item.artifact_id = make_ext_artifact_id(item.kind.empty() ? "native_plugin" : item.kind, manifest_path);
            return item;
        }

        const auto read_string = [&](std::string_view key) -> std::string {
            if (!parsed.contains(key) || !parsed[key].is_string()) {
                return {};
            }
            return trim_ascii(parsed[key].get<std::string>());
        };

        item.name = read_string("name");
        item.version = read_string("version");
        item.kind = read_string("kind");
        item.stage = read_string("stage");
        item.exclusive_group = read_string("exclusive_group");
        item.checksum = read_string("checksum");
        item.description = read_string("description");
        item.owner = read_string("owner");

        if (parsed.contains("priority") && parsed["priority"].is_number_unsigned()) {
            item.priority = parsed["priority"].get<std::uint32_t>();
        } else if (parsed.contains("priority") && parsed["priority"].is_number_integer()) {
            const auto signed_priority = parsed["priority"].get<long long>();
            if (signed_priority >= 0) {
                item.priority = static_cast<std::uint32_t>(signed_priority);
            } else {
                item.issues.push_back("priority_must_be_non_negative");
            }
        } else {
            item.issues.push_back("priority_missing");
        }

        if (parsed.contains("hook_scope")) {
            item.hook_scope = parse_json_string_list(parsed["hook_scope"]);
        }
        if (parsed.contains("target_profiles")) {
            item.target_profiles = parse_json_string_list(parsed["target_profiles"]);
        }
        if (parsed.contains("allowed_decisions")) {
            item.allowed_decisions = parse_json_string_list(parsed["allowed_decisions"]);
        }

        if (!is_ext_valid_kind(item.kind)) {
            item.issues.push_back("kind_invalid");
            if (!expected_kind.empty()) {
                item.kind = expected_kind;
            }
        }
        if (!expected_kind.empty() && !item.kind.empty() && item.kind != expected_kind) {
            item.issues.push_back("kind_mismatch");
        }
        if (item.name.empty()) {
            item.issues.push_back("name_missing");
        }
        if (item.version.empty()) {
            item.issues.push_back("version_missing");
        }
        if (!is_ext_valid_stage(item.stage)) {
            item.issues.push_back("stage_invalid");
        }
        if (item.exclusive_group.empty()) {
            item.issues.push_back("exclusive_group_missing");
        }
        if (item.hook_scope.empty()) {
            item.issues.push_back("hook_scope_missing");
        } else {
            for (const auto& hook : item.hook_scope) {
                if (!is_ext_valid_hook_scope(hook)) {
                    item.issues.push_back("hook_scope_invalid");
                    break;
                }
            }
        }

        if (!item.target_profiles.empty()) {
            for (const auto& profile : item.target_profiles) {
                if (!is_ext_valid_target_profile(profile)) {
                    item.issues.push_back("target_profile_invalid");
                    break;
                }
            }
        }

        if (!item.allowed_decisions.empty()) {
            std::int32_t prev_rank = -1;
            for (const auto& raw : item.allowed_decisions) {
                const std::string decision = to_lower_ascii(trim_ascii(raw));
                if (!is_ext_valid_terminal_decision(decision)) {
                    item.issues.push_back("terminal_decision_invalid");
                    continue;
                }
                const auto rank = terminal_decision_priority(decision);
                if (prev_rank > rank) {
                    item.issues.push_back("terminal_decision_precedence_invalid");
                }
                prev_rank = rank;
            }
            if (item.stage == "observe") {
                for (const auto& raw : item.allowed_decisions) {
                    const std::string decision = to_lower_ascii(trim_ascii(raw));
                    if (decision != "pass") {
                        item.issues.push_back("observe_stage_decision_forbidden");
                        break;
                    }
                }
            }
        }

        if (item.kind.empty()) {
            item.kind = expected_kind.empty() ? std::string("native_plugin") : expected_kind;
        }

        item.artifact_id = make_ext_artifact_id(item.kind, manifest_path);
        const auto artifact_path = derive_ext_artifact_path(manifest_path, item.kind);
        item.artifact_path = artifact_path.empty() ? std::string() : artifact_path.string();
        if (!artifact_path.empty()) {
            std::error_code ec;
            if (!std::filesystem::exists(artifact_path, ec) || ec) {
                item.issues.push_back("artifact_missing");
            }
        }

        return item;
    }

    void refresh_ext_inventory_cache() {
        std::vector<std::filesystem::path> manifests;
        for (const auto& root : {
                 resolve_ext_path(ext_plugins_dir_),
                 resolve_ext_path(ext_plugins_fallback_dir_),
                 resolve_ext_path(ext_scripts_dir_),
                 resolve_ext_path(ext_scripts_fallback_dir_)}) {
            auto found = scan_ext_manifest_paths_from_dir(root);
            manifests.insert(manifests.end(), found.begin(), found.end());
        }

        std::sort(manifests.begin(), manifests.end());
        manifests.erase(std::unique(manifests.begin(), manifests.end()), manifests.end());

        std::vector<ExtArtifactInventoryItem> items;
        items.reserve(manifests.size());
        std::size_t error_count = 0;
        for (const auto& manifest : manifests) {
            auto item = parse_ext_manifest_file(manifest);
            error_count += item.issues.size();
            items.push_back(std::move(item));
        }

        std::unordered_map<std::string, std::size_t> index;
        index.reserve(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto [_, inserted] = index.emplace(items[i].artifact_id, i);
            if (!inserted) {
                items[i].issues.push_back("artifact_id_collision");
            }
        }

        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            ext_inventory_cache_ = std::move(items);
            ext_inventory_index_ = std::move(index);
            ext_inventory_updated_at_ms_ = now_ms();
            ext_inventory_error_count_.store(static_cast<std::uint64_t>(error_count), std::memory_order_relaxed);
        }
    }

    const ExtArtifactInventoryItem* find_ext_inventory_item_locked(std::string_view artifact_id) const {
        const auto it = ext_inventory_index_.find(std::string(artifact_id));
        if (it == ext_inventory_index_.end()) {
            return nullptr;
        }
        if (it->second >= ext_inventory_cache_.size()) {
            return nullptr;
        }
        return &ext_inventory_cache_[it->second];
    }

    std::vector<server::core::state::InstanceRecord> resolve_ext_targets(const server::core::state::InstanceSelector& selector,
                                                                     bool selector_specified) const {
        std::vector<server::core::state::InstanceRecord> targets;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            targets = instances_cache_;
        }

        if (!selector_specified) {
            return targets;
        }
        return server::core::state::select_instances(targets, selector, nullptr);
    }

    bool parse_rollout_strategy_json(const nlohmann::json& root,
                                     ExtRolloutStrategy& rollout,
                                     std::string& error_message) const {
        rollout.type = "all_at_once";
        rollout.waves = {100};
        rollout.rollback_on_failure = false;

        if (!root.contains("rollout_strategy") || root["rollout_strategy"].is_null()) {
            return true;
        }
        if (!root["rollout_strategy"].is_object()) {
            error_message = "rollout_strategy must be an object";
            return false;
        }

        const auto& strategy = root["rollout_strategy"];
        if (strategy.contains("type")) {
            if (!strategy["type"].is_string()) {
                error_message = "rollout_strategy.type must be a string";
                return false;
            }
            rollout.type = to_lower_ascii(trim_ascii(strategy["type"].get<std::string>()));
        }

        if (strategy.contains("rollback_on_failure")) {
            if (strategy["rollback_on_failure"].is_boolean()) {
                rollout.rollback_on_failure = strategy["rollback_on_failure"].get<bool>();
            } else {
                error_message = "rollout_strategy.rollback_on_failure must be a boolean";
                return false;
            }
        }

        if (rollout.type == "all_at_once") {
            rollout.waves = {100};
            return true;
        }

        if (rollout.type != "canary_wave") {
            error_message = "rollout_strategy.type must be all_at_once or canary_wave";
            return false;
        }

        rollout.waves.clear();
        if (strategy.contains("waves") && strategy["waves"].is_array()) {
            for (const auto& wave : strategy["waves"]) {
                std::uint32_t value = 0;
                if (wave.is_number_unsigned()) {
                    value = wave.get<std::uint32_t>();
                } else if (wave.is_number_integer()) {
                    const auto signed_value = wave.get<long long>();
                    if (signed_value < 0) {
                        error_message = "rollout_strategy.waves must contain values 1..100";
                        return false;
                    }
                    value = static_cast<std::uint32_t>(signed_value);
                } else {
                    error_message = "rollout_strategy.waves must be numeric";
                    return false;
                }
                if (value == 0 || value > 100) {
                    error_message = "rollout_strategy.waves must contain values 1..100";
                    return false;
                }
                rollout.waves.push_back(value);
            }
        }

        if (rollout.waves.empty()) {
            rollout.waves = {5, 25, 100};
        }

        if (!std::is_sorted(rollout.waves.begin(), rollout.waves.end())) {
            error_message = "rollout_strategy.waves must be ascending";
            return false;
        }
        if (std::adjacent_find(rollout.waves.begin(), rollout.waves.end()) != rollout.waves.end()) {
            error_message = "rollout_strategy.waves must be strictly increasing";
            return false;
        }
        if (rollout.waves.back() != 100) {
            error_message = "rollout_strategy.waves last value must be 100";
            return false;
        }
        return true;
    }

    ExtCommandParseResult parse_ext_command_request(
        const server::core::metrics::MetricsHttpServer::HttpRequest& request,
        bool require_run_at) const {
        ExtCommandParseResult result;

        if (request.body.empty()) {
            result.error_message = "JSON body is required";
            return result;
        }

        std::string content_type;
        if (const auto it = request.headers.find("content-type"); it != request.headers.end()) {
            content_type = to_lower_ascii(trim_ascii(it->second));
        }
        if (content_type.rfind("application/json", 0) != 0) {
            result.error_message = "content-type must be application/json";
            return result;
        }

        const auto parsed = nlohmann::json::parse(request.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            result.error_message = "malformed JSON body";
            return result;
        }

        if (!parsed.contains("command_id") || !parsed["command_id"].is_string()) {
            result.error_message = "command_id is required";
            return result;
        }
        if (!parsed.contains("artifact_id") || !parsed["artifact_id"].is_string()) {
            result.error_message = "artifact_id is required";
            return result;
        }

        result.command.command_id = trim_ascii(parsed["command_id"].get<std::string>());
        result.command.artifact_id = trim_ascii(parsed["artifact_id"].get<std::string>());
        if (result.command.command_id.empty() || result.command.artifact_id.empty()) {
            result.error_message = "command_id and artifact_id must be non-empty";
            return result;
        }

        if (parsed.contains("selector") && !parsed["selector"].is_null()) {
            if (!parsed["selector"].is_object()) {
                result.error_message = "selector must be an object";
                return result;
            }

            const auto& selector_json = parsed["selector"];
            result.command.selector_specified = true;
            if (selector_json.contains("all")) {
                if (!selector_json["all"].is_boolean()) {
                    result.error_message = "selector.all must be boolean";
                    return result;
                }
                result.command.selector.all = selector_json["all"].get<bool>();
            }
            auto parse_array_field = [&](const char* key, std::vector<std::string>& out) -> bool {
                if (!selector_json.contains(key)) {
                    return true;
                }
                out = parse_json_string_list(selector_json[key]);
                if (!selector_json[key].is_array()) {
                    result.error_message = std::string("selector.") + key + " must be array";
                    return false;
                }
                return true;
            };

            if (!parse_array_field("server_ids", result.command.selector.server_ids)
                || !parse_array_field("roles", result.command.selector.roles)
                || !parse_array_field("game_modes", result.command.selector.game_modes)
                || !parse_array_field("regions", result.command.selector.regions)
                || !parse_array_field("shards", result.command.selector.shards)
                || !parse_array_field("tags", result.command.selector.tags)) {
                return result;
            }
        }

        if (parsed.contains("run_at_utc") && !parsed["run_at_utc"].is_null()) {
            std::uint64_t run_at = 0;
            if (parsed["run_at_utc"].is_number_unsigned()) {
                run_at = parsed["run_at_utc"].get<std::uint64_t>();
            } else if (parsed["run_at_utc"].is_number_integer()) {
                const auto signed_value = parsed["run_at_utc"].get<long long>();
                if (signed_value < 0) {
                    result.error_message = "run_at_utc must be >= 0";
                    return result;
                }
                run_at = static_cast<std::uint64_t>(signed_value);
            } else if (parsed["run_at_utc"].is_string()) {
                if (!parse_u64_strict(parsed["run_at_utc"].get<std::string>(), run_at)) {
                    result.error_message = "run_at_utc must be an unsigned integer";
                    return result;
                }
            } else {
                result.error_message = "run_at_utc must be null or unsigned integer";
                return result;
            }
            result.command.run_at_utc = run_at;
        }

        if (require_run_at && !result.command.run_at_utc.has_value()) {
            result.error_message = "run_at_utc is required for schedules";
            return result;
        }

        if (!parse_rollout_strategy_json(parsed, result.command.rollout, result.error_message)) {
            return result;
        }

        result.ok = true;
        return result;
    }

    ExtPrecheckResult run_ext_precheck_locked(const ExtCommandSpec& command) {
        ExtPrecheckResult result;
        result.ok = true;

        auto add_issue = [&](std::string code, std::string message) {
            result.ok = false;
            result.issues.push_back({std::move(code), std::move(message)});
        };

        const ExtArtifactInventoryItem* artifact = find_ext_inventory_item_locked(command.artifact_id);
        if (artifact == nullptr) {
            add_issue("artifact_not_found", "artifact_id not found in inventory");
            return result;
        }

        if (!artifact->issues.empty()) {
            add_issue("artifact_manifest_invalid", "artifact manifest has validation issues");
        }

        if (!is_ext_valid_stage(artifact->stage)) {
            add_issue("stage_invalid", "stage must be one of pre_validate|mutate|authorize|side_effect|observe");
        }

        if (artifact->hook_scope.empty()) {
            add_issue("hook_scope_missing", "artifact hook_scope is required");
        }

        if (artifact->exclusive_group.empty()) {
            add_issue("exclusive_group_missing", "artifact exclusive_group is required");
        }

        if (artifact->stage == "observe") {
            for (const auto& raw : artifact->allowed_decisions) {
                const std::string decision = to_lower_ascii(trim_ascii(raw));
                if (decision != "pass") {
                    add_issue("observe_stage_decision_forbidden", "observe stage artifacts must only allow pass");
                    break;
                }
            }
        }

        for (const auto& [existing_command_id, deployment] : ext_deployments_) {
            if (existing_command_id == command.command_id) {
                continue;
            }
            if (deployment.status == "failed" || deployment.status == "cancelled") {
                continue;
            }
            if (deployment.artifact_id == command.artifact_id) {
                continue;
            }
            if (deployment.stage != artifact->stage) {
                continue;
            }
            if (deployment.exclusive_group != artifact->exclusive_group) {
                continue;
            }

            bool hook_overlap = false;
            for (const auto& hook : artifact->hook_scope) {
                if (std::find(deployment.hook_scope.begin(), deployment.hook_scope.end(), hook)
                    != deployment.hook_scope.end()) {
                    hook_overlap = true;
                    break;
                }
            }
            if (!hook_overlap) {
                continue;
            }

            add_issue(
                "exclusive_group_conflict",
                "hook_scope/stage/exclusive_group conflict with command_id=" + existing_command_id);
        }

        auto targets = resolve_ext_targets(command.selector, command.selector_specified);
        result.target_count = targets.size();
        if (targets.empty()) {
            add_issue("target_empty", "selector does not match any live server instance");
        }

        return result;
    }

    bool save_ext_deployments_locked() {
        nlohmann::json root;
        root["version"] = 1;
        root["saved_at_ms"] = now_ms();
        root["deployments"] = nlohmann::json::array();

        std::vector<std::string> keys;
        keys.reserve(ext_deployments_.size());
        for (const auto& [key, _] : ext_deployments_) {
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());

        for (const auto& key : keys) {
            const auto it = ext_deployments_.find(key);
            if (it == ext_deployments_.end()) {
                continue;
            }
            const auto& d = it->second;

            nlohmann::json item;
            item["command_id"] = d.command_id;
            item["artifact_id"] = d.artifact_id;
            item["actor"] = d.actor;
            item["status"] = d.status;
            item["status_reason"] = d.status_reason;
            item["selector_specified"] = d.selector_specified;
            item["selector"] = {
                {"all", d.selector.all},
                {"server_ids", d.selector.server_ids},
                {"roles", d.selector.roles},
                {"game_modes", d.selector.game_modes},
                {"regions", d.selector.regions},
                {"shards", d.selector.shards},
                {"tags", d.selector.tags},
            };
            if (d.run_at_utc.has_value()) {
                item["run_at_utc"] = *d.run_at_utc;
            } else {
                item["run_at_utc"] = nullptr;
            }
            item["rollout_strategy"] = {
                {"type", d.rollout.type},
                {"waves", d.rollout.waves},
                {"rollback_on_failure", d.rollout.rollback_on_failure},
            };
            item["kind"] = d.kind;
            item["hook_scope"] = d.hook_scope;
            item["stage"] = d.stage;
            item["priority"] = d.priority;
            item["exclusive_group"] = d.exclusive_group;
            item["target_count"] = d.target_count;
            item["applied_targets"] = d.applied_targets;
            item["issues"] = nlohmann::json::array();
            for (const auto& issue : d.issues) {
                item["issues"].push_back({{"code", issue.code}, {"message", issue.message}});
            }
            item["created_at_ms"] = d.created_at_ms;
            item["updated_at_ms"] = d.updated_at_ms;

            root["deployments"].push_back(std::move(item));
        }

        const auto path = resolve_ext_path(ext_schedule_store_path_);
        const auto tmp_path = path.string() + ".tmp";

        std::error_code ec;
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            corelog::warn("admin_app failed to open ext deployment store temp file");
            return false;
        }
        out << root.dump(2);
        out.close();

        const auto backup_path = path.string() + ".bak";
        if (std::filesystem::exists(path, ec) && !ec) {
            ec.clear();
            std::filesystem::remove(backup_path, ec);
            ec.clear();
            std::filesystem::rename(path, backup_path, ec);
            if (ec) {
                corelog::warn(std::string("admin_app failed to backup ext deployment store: ") + ec.message());
            }
        }

        ec.clear();
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            corelog::warn(std::string("admin_app failed to persist ext deployment store: ") + ec.message());

            std::error_code restore_ec;
            if (std::filesystem::exists(backup_path, restore_ec) && !restore_ec) {
                std::filesystem::rename(backup_path, path, restore_ec);
                if (restore_ec) {
                    corelog::warn(std::string("admin_app failed to restore ext deployment backup: ") + restore_ec.message());
                }
            }
            return false;
        }

        std::filesystem::remove(backup_path, ec);

        ext_store_write_ok_total_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void load_ext_deployments_store() {
        const auto path = resolve_ext_path(ext_schedule_store_path_);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return;
        }

        std::ostringstream buf;
        buf << in.rdbuf();
        const auto parsed = nlohmann::json::parse(buf.str(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("deployments")
            || !parsed["deployments"].is_array()) {
            corelog::warn("admin_app failed to parse ext deployment store");
            ext_store_read_fail_total_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::unordered_map<std::string, ExtDeploymentRecord> loaded;
        for (const auto& item : parsed["deployments"]) {
            if (!item.is_object()) {
                continue;
            }
            if (!item.contains("command_id") || !item["command_id"].is_string()) {
                continue;
            }

            ExtDeploymentRecord d;
            d.command_id = trim_ascii(item["command_id"].get<std::string>());
            if (d.command_id.empty()) {
                continue;
            }
            if (item.contains("artifact_id") && item["artifact_id"].is_string()) {
                d.artifact_id = trim_ascii(item["artifact_id"].get<std::string>());
            }
            if (item.contains("actor") && item["actor"].is_string()) {
                d.actor = trim_ascii(item["actor"].get<std::string>());
            }
            if (item.contains("status") && item["status"].is_string()) {
                d.status = ext_status_text(item["status"].get<std::string>());
            }
            if (item.contains("status_reason") && item["status_reason"].is_string()) {
                d.status_reason = trim_ascii(item["status_reason"].get<std::string>());
            }
            if (item.contains("selector_specified") && item["selector_specified"].is_boolean()) {
                d.selector_specified = item["selector_specified"].get<bool>();
            }
            if (item.contains("selector") && item["selector"].is_object()) {
                const auto& selector = item["selector"];
                if (selector.contains("all") && selector["all"].is_boolean()) {
                    d.selector.all = selector["all"].get<bool>();
                }
                if (selector.contains("server_ids")) {
                    d.selector.server_ids = parse_json_string_list(selector["server_ids"]);
                }
                if (selector.contains("roles")) {
                    d.selector.roles = parse_json_string_list(selector["roles"]);
                }
                if (selector.contains("game_modes")) {
                    d.selector.game_modes = parse_json_string_list(selector["game_modes"]);
                }
                if (selector.contains("regions")) {
                    d.selector.regions = parse_json_string_list(selector["regions"]);
                }
                if (selector.contains("shards")) {
                    d.selector.shards = parse_json_string_list(selector["shards"]);
                }
                if (selector.contains("tags")) {
                    d.selector.tags = parse_json_string_list(selector["tags"]);
                }
            }
            if (item.contains("run_at_utc") && !item["run_at_utc"].is_null()) {
                if (item["run_at_utc"].is_number_unsigned()) {
                    d.run_at_utc = item["run_at_utc"].get<std::uint64_t>();
                } else if (item["run_at_utc"].is_number_integer()) {
                    const auto signed_value = item["run_at_utc"].get<long long>();
                    if (signed_value >= 0) {
                        d.run_at_utc = static_cast<std::uint64_t>(signed_value);
                    }
                }
            }

            if (item.contains("rollout_strategy") && item["rollout_strategy"].is_object()) {
                const auto& rollout = item["rollout_strategy"];
                if (rollout.contains("type") && rollout["type"].is_string()) {
                    d.rollout.type = to_lower_ascii(trim_ascii(rollout["type"].get<std::string>()));
                }
                if (rollout.contains("waves") && rollout["waves"].is_array()) {
                    d.rollout.waves.clear();
                    for (const auto& wave : rollout["waves"]) {
                        if (wave.is_number_unsigned()) {
                            d.rollout.waves.push_back(wave.get<std::uint32_t>());
                        } else if (wave.is_number_integer()) {
                            const auto signed_value = wave.get<long long>();
                            if (signed_value >= 0) {
                                d.rollout.waves.push_back(static_cast<std::uint32_t>(signed_value));
                            }
                        }
                    }
                }
                if (rollout.contains("rollback_on_failure") && rollout["rollback_on_failure"].is_boolean()) {
                    d.rollout.rollback_on_failure = rollout["rollback_on_failure"].get<bool>();
                }
            }
            if (d.rollout.waves.empty()) {
                d.rollout.waves = (d.rollout.type == "canary_wave") ? std::vector<std::uint32_t>{5, 25, 100}
                                                                      : std::vector<std::uint32_t>{100};
            }

            if (item.contains("kind") && item["kind"].is_string()) {
                d.kind = trim_ascii(item["kind"].get<std::string>());
            }
            if (item.contains("hook_scope")) {
                d.hook_scope = parse_json_string_list(item["hook_scope"]);
            }
            if (item.contains("stage") && item["stage"].is_string()) {
                d.stage = trim_ascii(item["stage"].get<std::string>());
            }
            if (item.contains("priority") && item["priority"].is_number_unsigned()) {
                d.priority = item["priority"].get<std::uint32_t>();
            }
            if (item.contains("exclusive_group") && item["exclusive_group"].is_string()) {
                d.exclusive_group = trim_ascii(item["exclusive_group"].get<std::string>());
            }
            if (item.contains("target_count") && item["target_count"].is_number_unsigned()) {
                d.target_count = static_cast<std::size_t>(item["target_count"].get<std::uint64_t>());
            }
            if (item.contains("applied_targets") && item["applied_targets"].is_number_unsigned()) {
                d.applied_targets = static_cast<std::size_t>(item["applied_targets"].get<std::uint64_t>());
            }
            if (item.contains("issues") && item["issues"].is_array()) {
                for (const auto& issue_json : item["issues"]) {
                    if (!issue_json.is_object()) {
                        continue;
                    }
                    ExtPrecheckIssue issue;
                    if (issue_json.contains("code") && issue_json["code"].is_string()) {
                        issue.code = issue_json["code"].get<std::string>();
                    }
                    if (issue_json.contains("message") && issue_json["message"].is_string()) {
                        issue.message = issue_json["message"].get<std::string>();
                    }
                    if (!issue.code.empty() || !issue.message.empty()) {
                        d.issues.push_back(std::move(issue));
                    }
                }
            }
            if (item.contains("created_at_ms") && item["created_at_ms"].is_number_unsigned()) {
                d.created_at_ms = item["created_at_ms"].get<std::uint64_t>();
            }
            if (item.contains("updated_at_ms") && item["updated_at_ms"].is_number_unsigned()) {
                d.updated_at_ms = item["updated_at_ms"].get<std::uint64_t>();
            }

            loaded[d.command_id] = std::move(d);
        }

        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            ext_deployments_ = std::move(loaded);
        }
        ext_store_read_ok_total_.fetch_add(1, std::memory_order_relaxed);
    }

    bool publish_ext_wave_command(const ExtDeploymentRecord& deployment,
                                  const std::vector<std::string>& target_ids,
                                  std::size_t wave_index,
                                  std::uint32_t wave_percent,
                                  bool rollback) {
        if (!redis_ || !redis_available_.load(std::memory_order_relaxed)) {
            return false;
        }

        const std::string channel = redis_channel_prefix_ + (rollback ? "fanout:admin:ext:rollback" : "fanout:admin:ext:deploy");
        std::unordered_map<std::string, std::string> fields;
        fields["op"] = rollback ? "ext_rollback" : "ext_deploy";
        fields["actor"] = sanitize_single_line(deployment.actor);
        fields["request_id"] = "admin-ext-" + deployment.command_id + "-wave-" + std::to_string(wave_index + 1);
        fields["command_id"] = deployment.command_id;
        fields["artifact_id"] = deployment.artifact_id;
        fields["kind"] = deployment.kind;
        fields["stage"] = deployment.stage;
        fields["priority"] = std::to_string(deployment.priority);
        fields["exclusive_group"] = deployment.exclusive_group;
        fields["hook_scope"] = join_csv(deployment.hook_scope);
        fields["rollout_type"] = deployment.rollout.type;
        fields["wave_index"] = std::to_string(wave_index + 1);
        fields["wave_percent"] = std::to_string(wave_percent);
        fields["server_ids"] = join_csv(target_ids);
        fields["all"] = "0";
        if (deployment.run_at_utc.has_value()) {
            fields["run_at_utc"] = std::to_string(*deployment.run_at_utc);
        }

        const auto message = build_signed_admin_message(std::move(fields), request_seq_.fetch_add(1, std::memory_order_relaxed));
        if (!message) {
            return false;
        }
        return redis_->publish(channel, *message);
    }

    void process_ext_deployment_queue() {
        ext_scheduler_runs_total_.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t now = now_ms();

        std::vector<std::string> due_commands;
        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            due_commands.reserve(ext_deployments_.size());
            for (const auto& [command_id, deployment] : ext_deployments_) {
                if (deployment.status != "pending" && deployment.status != "precheck_passed") {
                    continue;
                }
                if (!deployment.run_at_utc.has_value()) {
                    due_commands.push_back(command_id);
                    continue;
                }
                if (now < *deployment.run_at_utc) {
                    continue;
                }
                due_commands.push_back(command_id);
            }
        }

        if (!due_commands.empty()) {
            ext_scheduler_due_total_.fetch_add(
                static_cast<std::uint64_t>(due_commands.size()),
                std::memory_order_relaxed);
        }

        for (const auto& command_id : due_commands) {
            execute_ext_deployment(command_id, now);
        }
    }

    void execute_ext_deployment(const std::string& command_id, std::uint64_t now) {
        ExtDeploymentRecord deployment;
        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            const auto it = ext_deployments_.find(command_id);
            if (it == ext_deployments_.end()) {
                return;
            }
            if (it->second.status != "pending" && it->second.status != "precheck_passed") {
                return;
            }

            if (it->second.run_at_utc.has_value()
                && now > (*it->second.run_at_utc + static_cast<std::uint64_t>(ext_max_clock_skew_ms_))) {
                it->second.status = "failed";
                it->second.status_reason = "clock_skew";
                it->second.updated_at_ms = now;
                it->second.issues.push_back({"clock_skew", "run_at_utc exceeded max_clock_skew_ms"});
                ext_deployments_failed_total_.fetch_add(1, std::memory_order_relaxed);
                ext_clock_skew_failed_total_.fetch_add(1, std::memory_order_relaxed);
                save_ext_deployments_locked();
                return;
            }

            it->second.status = "executing";
            it->second.updated_at_ms = now;
            deployment = it->second;
            save_ext_deployments_locked();
        }

        auto targets = resolve_ext_targets(deployment.selector, deployment.selector_specified);
        std::sort(targets.begin(), targets.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.instance_id < rhs.instance_id;
        });

        if (targets.empty()) {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            if (auto it = ext_deployments_.find(command_id); it != ext_deployments_.end()) {
                it->second.status = "failed";
                it->second.status_reason = "target_empty";
                it->second.updated_at_ms = now_ms();
                it->second.issues.push_back({"target_empty", "selector does not match any live server instance"});
                ext_deployments_failed_total_.fetch_add(1, std::memory_order_relaxed);
                save_ext_deployments_locked();
            }
            return;
        }

        const std::size_t total_targets = targets.size();
        std::size_t applied_count = 0;
        std::vector<std::string> applied_ids;
        applied_ids.reserve(total_targets);

        bool failed = false;
        ExtPrecheckIssue failure_issue;

        for (std::size_t wave_index = 0; wave_index < deployment.rollout.waves.size(); ++wave_index) {
            const std::uint32_t wave_percent = deployment.rollout.waves[wave_index];
            const std::size_t wave_target_end = std::max<std::size_t>(
                applied_count,
                static_cast<std::size_t>(std::ceil((static_cast<double>(total_targets) * wave_percent) / 100.0)));
            const std::size_t bounded_end = std::min<std::size_t>(wave_target_end, total_targets);
            if (bounded_end <= applied_count) {
                continue;
            }

            if (ext_force_fail_wave_index_ > 0
                && static_cast<std::uint32_t>(wave_index + 1) == ext_force_fail_wave_index_) {
                failed = true;
                failure_issue = {
                    "wave_forced_failure",
                    "forced failure at wave " + std::to_string(wave_index + 1)
                        + " by ADMIN_EXT_FORCE_FAIL_WAVE_INDEX"};
                ext_wave_failed_total_.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            std::vector<std::string> wave_targets;
            wave_targets.reserve(bounded_end - applied_count);
            for (std::size_t i = applied_count; i < bounded_end; ++i) {
                wave_targets.push_back(targets[i].instance_id);
            }

            if (!publish_ext_wave_command(deployment, wave_targets, wave_index, wave_percent, false)) {
                failed = true;
                failure_issue = {
                    "wave_publish_failed",
                    "failed to publish deployment wave " + std::to_string(wave_index + 1)};
                ext_wave_failed_total_.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            applied_ids.insert(applied_ids.end(), wave_targets.begin(), wave_targets.end());
            applied_count = bounded_end;
            ext_deployment_wave_total_.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            const auto it = ext_deployments_.find(command_id);
            if (it == ext_deployments_.end()) {
                return;
            }

            it->second.target_count = total_targets;
            it->second.applied_targets = applied_count;
            it->second.updated_at_ms = now_ms();

            if (failed) {
                it->second.status = "failed";
                it->second.status_reason = failure_issue.code;
                it->second.issues.push_back(failure_issue);
                ext_deployments_failed_total_.fetch_add(1, std::memory_order_relaxed);

                if (it->second.rollout.rollback_on_failure && !applied_ids.empty()) {
                    const bool rollback_ok = publish_ext_wave_command(
                        it->second,
                        applied_ids,
                        0,
                        100,
                        true);
                    if (rollback_ok) {
                        ext_rollbacks_total_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else {
                it->second.status = "completed";
                it->second.status_reason.clear();
                ext_deployments_completed_total_.fetch_add(1, std::memory_order_relaxed);
            }

            save_ext_deployments_locked();
        }
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

        stream << "# TYPE admin_instances_selector_requests_total counter\n";
        stream << "admin_instances_selector_requests_total "
               << instances_selector_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_instances_selector_mismatch_total counter\n";
        stream << "admin_instances_selector_mismatch_total "
               << instances_selector_mismatch_total_.load(std::memory_order_relaxed) << "\n";

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

        stream << "# TYPE admin_command_signing_errors_total counter\n";
        stream << "admin_command_signing_errors_total "
               << command_signing_errors_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_inventory_requests_total counter\n";
        stream << "admin_ext_inventory_requests_total "
               << ext_inventory_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_precheck_requests_total counter\n";
        stream << "admin_ext_precheck_requests_total "
               << ext_precheck_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_deployments_requests_total counter\n";
        stream << "admin_ext_deployments_requests_total "
               << ext_deployments_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_schedules_requests_total counter\n";
        stream << "admin_ext_schedules_requests_total "
               << ext_schedules_requests_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_precheck_failed_total counter\n";
        stream << "admin_ext_precheck_failed_total "
               << ext_precheck_failed_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_command_id_conflict_total counter\n";
        stream << "admin_ext_command_id_conflict_total "
               << ext_command_id_conflict_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_scheduler_runs_total counter\n";
        stream << "admin_ext_scheduler_runs_total "
               << ext_scheduler_runs_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_scheduler_due_total counter\n";
        stream << "admin_ext_scheduler_due_total "
               << ext_scheduler_due_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_deployment_wave_total counter\n";
        stream << "admin_ext_deployment_wave_total "
               << ext_deployment_wave_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_wave_failed_total counter\n";
        stream << "admin_ext_wave_failed_total "
               << ext_wave_failed_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_rollbacks_total counter\n";
        stream << "admin_ext_rollbacks_total "
               << ext_rollbacks_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_clock_skew_failed_total counter\n";
        stream << "admin_ext_clock_skew_failed_total "
               << ext_clock_skew_failed_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_deployments_completed_total counter\n";
        stream << "admin_ext_deployments_completed_total "
               << ext_deployments_completed_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_deployments_failed_total counter\n";
        stream << "admin_ext_deployments_failed_total "
               << ext_deployments_failed_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_store_read_ok_total counter\n";
        stream << "admin_ext_store_read_ok_total "
               << ext_store_read_ok_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_store_read_fail_total counter\n";
        stream << "admin_ext_store_read_fail_total "
               << ext_store_read_fail_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_ext_store_write_ok_total counter\n";
        stream << "admin_ext_store_write_ok_total "
               << ext_store_write_ok_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_instances_cached gauge\n";
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            stream << "admin_instances_cached " << instances_cache_.size() << "\n";
        }

        stream << "# TYPE admin_ext_inventory_cached gauge\n";
        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            stream << "admin_ext_inventory_cached " << ext_inventory_cache_.size() << "\n";
            stream << "# TYPE admin_ext_deployments_cached gauge\n";
            stream << "admin_ext_deployments_cached " << ext_deployments_.size() << "\n";
        }

        stream << "# TYPE admin_ext_inventory_errors_total gauge\n";
        stream << "admin_ext_inventory_errors_total "
               << ext_inventory_error_count_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE admin_redis_available gauge\n";
        stream << "admin_redis_available " << (redis_available_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

        stream << "# TYPE admin_worker_metrics_available gauge\n";
        stream << "admin_worker_metrics_available " << (worker_available_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

        stream << "# TYPE admin_read_only_mode gauge\n";
        stream << "admin_read_only_mode " << (admin_read_only_ ? 1 : 0) << "\n";

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
        const bool is_ext_inventory = (path == "/api/v1/ext/inventory");
        const bool is_ext_precheck = (path == "/api/v1/ext/precheck");
        const bool is_ext_deployments = (path == "/api/v1/ext/deployments");
        const bool is_ext_schedules = (path == "/api/v1/ext/schedules");
        const bool is_mute = (path == "/api/v1/users/mute");
        const bool is_unmute = (path == "/api/v1/users/unmute");
        const bool is_ban = (path == "/api/v1/users/ban");
        const bool is_unban = (path == "/api/v1/users/unban");
        const bool is_kick = (path == "/api/v1/users/kick");
        const bool is_user_moderation = is_mute || is_unmute || is_ban || is_unban || is_kick;
        const bool is_legacy_write_endpoint = is_disconnect || is_announce || is_settings || is_user_moderation;
        const bool is_ext_deployments_write = is_ext_deployments && method == "POST";
        const bool is_ext_write_endpoint = is_ext_precheck || is_ext_deployments_write || is_ext_schedules;
        const bool is_readonly_blocked_endpoint = is_legacy_write_endpoint || is_ext_deployments_write || is_ext_schedules;
        const bool is_write_endpoint = is_legacy_write_endpoint || is_ext_write_endpoint;

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

        if (is_ext_inventory && method != "GET") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "ext inventory endpoint requires GET"));
        }

        if (is_ext_precheck && method != "POST") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "ext precheck endpoint requires POST"));
        }

        if (is_ext_deployments && method != "GET" && method != "POST") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "ext deployments endpoint supports GET and POST"));
        }

        if (is_ext_schedules && method != "POST") {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "405 Method Not Allowed",
                "METHOD_NOT_ALLOWED",
                "ext schedules endpoint requires POST"));
        }

        if (is_readonly_blocked_endpoint && admin_read_only_) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(json_error(
                request_id,
                "403 Forbidden",
                "READ_ONLY",
                "write endpoints are disabled by ADMIN_READ_ONLY"));
        }

        QueryOptions query_options;
        std::string write_query_string;
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

        if (is_legacy_write_endpoint) {
            const WriteParamsResult write_params = merge_write_params_from_request(split.query, request);
            if (!write_params.ok) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return finalize(json_error(
                    request_id,
                    write_params.error_status,
                    write_params.error_code,
                    write_params.error_message));
            }
            write_query_string = write_params.merged_query;
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
            return finalize(handle_instances(request_id, query_options, split.query));
        }

        constexpr std::string_view kInstancesPrefix = "/api/v1/instances/";
        if (path.starts_with(kInstancesPrefix)) {
            instances_requests_total_.fetch_add(1, std::memory_order_relaxed);
            const std::string id = std::string(path.substr(kInstancesPrefix.size()));
            return finalize(handle_instance_detail(request_id, id, query_options));
        }

        if (is_ext_inventory) {
            ext_inventory_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_ext_inventory(request_id, query_options, split.query));
        }

        if (is_ext_precheck) {
            if (auto forbidden = require_role(kRoleRequiredSettings)) {
                return finalize(std::move(*forbidden));
            }
            ext_precheck_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_ext_precheck(request_id, auth, request));
        }

        if (is_ext_deployments) {
            ext_deployments_requests_total_.fetch_add(1, std::memory_order_relaxed);
            if (method == "GET") {
                return finalize(handle_ext_deployments(request_id, query_options, split.query));
            }
            if (auto forbidden = require_role(kRoleRequiredSettings)) {
                return finalize(std::move(*forbidden));
            }
            return finalize(handle_ext_deployment_submit(request_id, auth, request, false));
        }

        if (is_ext_schedules) {
            if (auto forbidden = require_role(kRoleRequiredSettings)) {
                return finalize(std::move(*forbidden));
            }
            ext_schedules_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_ext_deployment_submit(request_id, auth, request, true));
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
            if (auto forbidden = require_role(kRoleRequiredDisconnect)) {
                return finalize(std::move(*forbidden));
            }
            disconnect_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_disconnect_users(request_id, auth, write_query_string));
        }

        if (is_announce) {
            if (auto forbidden = require_role(kRoleRequiredAnnouncement)) {
                return finalize(std::move(*forbidden));
            }
            announce_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_announcement(request_id, auth, write_query_string));
        }

        if (is_settings) {
            if (auto forbidden = require_role(kRoleRequiredSettings)) {
                return finalize(std::move(*forbidden));
            }
            settings_requests_total_.fetch_add(1, std::memory_order_relaxed);
            return finalize(handle_runtime_setting(request_id, auth, write_query_string));
        }

        if (is_user_moderation) {
            if (auto forbidden = require_role(kRoleRequiredModeration)) {
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
            return finalize(handle_user_moderation(request_id, auth, op, write_query_string));
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
        data << "\"read_only\":" << bool_json(admin_read_only_) << ",";

        const bool allow_disconnect = !admin_read_only_ && has_min_role(auth.role, kRoleRequiredDisconnect);
        const bool allow_announce = !admin_read_only_ && has_min_role(auth.role, kRoleRequiredAnnouncement);
        const bool allow_settings = !admin_read_only_ && has_min_role(auth.role, kRoleRequiredSettings);
        const bool allow_moderation = !admin_read_only_ && has_min_role(auth.role, kRoleRequiredModeration);
        const bool allow_ext_deploy = !admin_read_only_ && has_min_role(auth.role, kRoleRequiredSettings);

        data << "\"capabilities\":{";
        data << "\"disconnect\":" << bool_json(allow_disconnect) << ",";
        data << "\"announce\":" << bool_json(allow_announce) << ",";
        data << "\"settings\":" << bool_json(allow_settings) << ",";
        data << "\"moderation\":" << bool_json(allow_moderation) << ",";
        data << "\"ext_deploy\":" << bool_json(allow_ext_deploy);
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
        data << "\"read_only\":" << bool_json(admin_read_only_) << ",";
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
        data << "\"worker\":\"/api/v1/worker/write-behind\",";
        data << "\"ext_inventory\":\"/api/v1/ext/inventory\",";
        data << "\"ext_deployments\":\"/api/v1/ext/deployments\"";
        data << "}";
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_instances(std::uint64_t request_id,
                     const QueryOptions& options,
                     std::string_view selector_query) {
        if (!registry_backend_ || !redis_available_.load(std::memory_order_relaxed)) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "UPSTREAM_UNAVAILABLE",
                "redis registry unavailable");
        }

        std::vector<server::core::state::InstanceRecord> items;
        std::uint64_t updated_ms = 0;
        server::core::state::SelectorMatchStats selector_stats{};
        bool selector_applied = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            items = instances_cache_;
            updated_ms = instances_updated_at_ms_;
        }

        const SelectorParseResult selector_parse = parse_instance_selector_query(selector_query);
        if (!selector_parse.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                selector_parse.error_message,
                "{" +
                    std::string("\"parameter\":\"") + json_escape(selector_parse.error_param) +
                    "\",\"value\":\"" + json_escape(selector_parse.error_value) + "\"}");
        }
        if (selector_parse.selector_specified) {
            selector_applied = true;
            items = server::core::state::select_instances(items, selector_parse.selector, &selector_stats);
            instances_selector_requests_total_.fetch_add(1, std::memory_order_relaxed);
            instances_selector_mismatch_total_.fetch_add(selector_stats.selector_mismatch, std::memory_order_relaxed);
        }
        const auto selector_layer = server::core::state::classify_selector_policy_layer(selector_parse.selector);

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
            data << "\"game_mode\":\"" << json_escape(it.game_mode) << "\",";
            data << "\"region\":\"" << json_escape(it.region) << "\",";
            data << "\"shard\":\"" << json_escape(it.shard) << "\",";
            data << "\"tags\":[";
            for (std::size_t tag_index = 0; tag_index < it.tags.size(); ++tag_index) {
                if (tag_index != 0) {
                    data << ",";
                }
                data << "\"" << json_escape(it.tags[tag_index]) << "\"";
            }
            data << "],";
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
        data << "\"selector\":{";
        data << "\"applied\":" << bool_json(selector_applied) << ",";
        data << "\"layer\":\"" << server::core::state::selector_policy_layer_name(selector_layer) << "\",";
        data << "\"scanned\":" << selector_stats.scanned << ",";
        data << "\"matched\":" << selector_stats.matched << ",";
        data << "\"mismatch\":" << selector_stats.selector_mismatch;
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

        std::optional<server::core::state::InstanceRecord> item;
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
        data << "\"game_mode\":\"" << json_escape(item->game_mode) << "\",";
        data << "\"region\":\"" << json_escape(item->region) << "\",";
        data << "\"shard\":\"" << json_escape(item->shard) << "\",";
        data << "\"tags\":[";
        for (std::size_t i = 0; i < item->tags.size(); ++i) {
            if (i != 0) {
                data << ",";
            }
            data << "\"" << json_escape(item->tags[i]) << "\"";
        }
        data << "],";
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
    handle_ext_inventory(std::uint64_t request_id,
                         const QueryOptions& options,
                         std::string_view query_string) {
        (void)options;
        const auto params = parse_query_string(query_string);

        std::string kind_filter;
        std::string stage_filter;
        std::string hook_filter;
        std::string target_profile_filter;

        if (const auto it = params.find("kind"); it != params.end()) {
            kind_filter = to_lower_ascii(trim_ascii(it->second));
        }
        if (const auto it = params.find("stage"); it != params.end()) {
            stage_filter = to_lower_ascii(trim_ascii(it->second));
        }
        if (const auto it = params.find("hook_scope"); it != params.end()) {
            hook_filter = to_lower_ascii(trim_ascii(it->second));
        }
        if (const auto it = params.find("target_profile"); it != params.end()) {
            target_profile_filter = to_lower_ascii(trim_ascii(it->second));
        }

        std::vector<ExtArtifactInventoryItem> inventory;
        std::uint64_t updated_at_ms = 0;
        std::uint64_t error_count = 0;
        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            inventory = ext_inventory_cache_;
            updated_at_ms = ext_inventory_updated_at_ms_;
            error_count = ext_inventory_error_count_.load(std::memory_order_relaxed);
        }

        std::vector<ExtArtifactInventoryItem> filtered;
        filtered.reserve(inventory.size());
        for (const auto& item : inventory) {
            if (!kind_filter.empty() && to_lower_ascii(item.kind) != kind_filter) {
                continue;
            }
            if (!stage_filter.empty() && to_lower_ascii(item.stage) != stage_filter) {
                continue;
            }
            if (!hook_filter.empty()) {
                bool has_hook = false;
                for (const auto& hook : item.hook_scope) {
                    if (to_lower_ascii(hook) == hook_filter) {
                        has_hook = true;
                        break;
                    }
                }
                if (!has_hook) {
                    continue;
                }
            }
            if (!target_profile_filter.empty()) {
                bool has_profile = false;
                for (const auto& profile : item.target_profiles) {
                    if (to_lower_ascii(profile) == target_profile_filter) {
                        has_profile = true;
                        break;
                    }
                }
                if (!has_profile) {
                    continue;
                }
            }
            filtered.push_back(item);
        }

        std::sort(filtered.begin(), filtered.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.artifact_id < rhs.artifact_id;
        });

        std::ostringstream data;
        data << "{";
        data << "\"items\":[";
        for (std::size_t i = 0; i < filtered.size(); ++i) {
            const auto& item = filtered[i];
            if (i != 0) {
                data << ",";
            }
            data << "{";
            data << "\"artifact_id\":\"" << json_escape(item.artifact_id) << "\",";
            data << "\"kind\":\"" << json_escape(item.kind) << "\",";
            data << "\"name\":\"" << json_escape(item.name) << "\",";
            data << "\"version\":\"" << json_escape(item.version) << "\",";
            data << "\"hook_scope\":[";
            for (std::size_t j = 0; j < item.hook_scope.size(); ++j) {
                if (j != 0) {
                    data << ",";
                }
                data << "\"" << json_escape(item.hook_scope[j]) << "\"";
            }
            data << "],";
            data << "\"stage\":\"" << json_escape(item.stage) << "\",";
            data << "\"priority\":" << item.priority << ",";
            data << "\"exclusive_group\":\"" << json_escape(item.exclusive_group) << "\",";
            data << "\"checksum\":\"" << json_escape(item.checksum) << "\",";
            data << "\"target_profiles\":[";
            for (std::size_t j = 0; j < item.target_profiles.size(); ++j) {
                if (j != 0) {
                    data << ",";
                }
                data << "\"" << json_escape(item.target_profiles[j]) << "\"";
            }
            data << "],";
            data << "\"description\":\"" << json_escape(item.description) << "\",";
            data << "\"owner\":\"" << json_escape(item.owner) << "\",";
            data << "\"issues\":[";
            for (std::size_t j = 0; j < item.issues.size(); ++j) {
                if (j != 0) {
                    data << ",";
                }
                data << "\"" << json_escape(item.issues[j]) << "\"";
            }
            data << "]";
            data << "}";
        }
        data << "],";
        data << "\"count\":" << filtered.size() << ",";
        data << "\"inventory_errors\":" << error_count << ",";
        data << "\"updated_at_ms\":" << updated_at_ms;
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_ext_precheck(std::uint64_t request_id,
                        const AuthContext& auth,
                        const server::core::metrics::MetricsHttpServer::HttpRequest& request) {
        const auto parsed = parse_ext_command_request(request, false);
        if (!parsed.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "400 Bad Request", "BAD_REQUEST", parsed.error_message);
        }

        ExtPrecheckResult precheck;
        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            if (ext_deployments_.find(parsed.command.command_id) != ext_deployments_.end()) {
                ext_command_id_conflict_total_.fetch_add(1, std::memory_order_relaxed);
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "409 Conflict",
                    "IDEMPOTENT_REJECTED",
                    "command_id already exists and cannot be reused");
            }

            precheck = run_ext_precheck_locked(parsed.command);
            ext_precheck_cache_[parsed.command.command_id] = ExtPrecheckRecord{
                parsed.command,
                precheck,
                now_ms()};
        }

        if (!precheck.ok) {
            ext_precheck_failed_total_.fetch_add(1, std::memory_order_relaxed);
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);

            std::ostringstream details;
            details << "{";
            details << "\"command_id\":\"" << json_escape(parsed.command.command_id) << "\",";
            details << "\"status\":\"failed\",";
            details << "\"target_count\":" << precheck.target_count << ",";
            details << "\"issues\":[";
            for (std::size_t i = 0; i < precheck.issues.size(); ++i) {
                if (i != 0) {
                    details << ",";
                }
                details << "{";
                details << "\"code\":\"" << json_escape(precheck.issues[i].code) << "\",";
                details << "\"message\":\"" << json_escape(precheck.issues[i].message) << "\"";
                details << "}";
            }
            details << "]";
            details << "}";

            return json_error(
                request_id,
                "409 Conflict",
                "PRECHECK_FAILED",
                "precheck failed; deployment blocked",
                details.str());
        }

        std::ostringstream data;
        data << "{";
        data << "\"command_id\":\"" << json_escape(parsed.command.command_id) << "\",";
        data << "\"status\":\"precheck_passed\",";
        data << "\"target_count\":" << precheck.target_count << ",";
        data << "\"issues\":[],";
        data << "\"actor\":\"" << json_escape(auth.actor) << "\"";
        data << "}";

        return json_ok(request_id, data.str());
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_ext_deployment_submit(std::uint64_t request_id,
                                 const AuthContext& auth,
                                 const server::core::metrics::MetricsHttpServer::HttpRequest& request,
                                 bool require_schedule_time) {
        auto parsed = parse_ext_command_request(request, require_schedule_time);
        if (!parsed.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(request_id, "400 Bad Request", "BAD_REQUEST", parsed.error_message);
        }

        const std::uint64_t now = now_ms();
        if (!require_schedule_time && parsed.command.run_at_utc.has_value()) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                "run_at_utc must be null or omitted for deployments");
        }
        if (!require_schedule_time) {
            parsed.command.run_at_utc = now;
        }

        ExtPrecheckResult precheck;
        const ExtArtifactInventoryItem* artifact = nullptr;
        {
            std::lock_guard<std::mutex> lock(ext_mutex_);

            if (ext_deployments_.find(parsed.command.command_id) != ext_deployments_.end()) {
                ext_command_id_conflict_total_.fetch_add(1, std::memory_order_relaxed);
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "409 Conflict",
                    "IDEMPOTENT_REJECTED",
                    "command_id already exists and cannot be reused");
            }

            precheck = run_ext_precheck_locked(parsed.command);
            ext_precheck_cache_[parsed.command.command_id] = ExtPrecheckRecord{
                parsed.command,
                precheck,
                now};

            if (!precheck.ok) {
                ext_precheck_failed_total_.fetch_add(1, std::memory_order_relaxed);
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);

                std::ostringstream details;
                details << "{";
                details << "\"command_id\":\"" << json_escape(parsed.command.command_id) << "\",";
                details << "\"status\":\"failed\",";
                details << "\"issues\":[";
                for (std::size_t i = 0; i < precheck.issues.size(); ++i) {
                    if (i != 0) {
                        details << ",";
                    }
                    details << "{";
                    details << "\"code\":\"" << json_escape(precheck.issues[i].code) << "\",";
                    details << "\"message\":\"" << json_escape(precheck.issues[i].message) << "\"";
                    details << "}";
                }
                details << "]";
                details << "}";

                return json_error(
                    request_id,
                    "409 Conflict",
                    "PRECHECK_FAILED",
                    "precheck failed; deployment blocked",
                    details.str());
            }

            artifact = find_ext_inventory_item_locked(parsed.command.artifact_id);
            if (artifact == nullptr) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "409 Conflict",
                    "PRECHECK_FAILED",
                    "artifact_id not found in inventory");
            }

            ExtDeploymentRecord deployment;
            deployment.command_id = parsed.command.command_id;
            deployment.artifact_id = parsed.command.artifact_id;
            deployment.actor = sanitize_single_line(auth.actor);
            deployment.status = "pending";
            deployment.selector = parsed.command.selector;
            deployment.selector_specified = parsed.command.selector_specified;
            deployment.run_at_utc = parsed.command.run_at_utc;
            deployment.rollout = parsed.command.rollout;
            deployment.kind = artifact->kind;
            deployment.hook_scope = artifact->hook_scope;
            deployment.stage = artifact->stage;
            deployment.priority = artifact->priority;
            deployment.exclusive_group = artifact->exclusive_group;
            deployment.target_count = precheck.target_count;
            deployment.applied_targets = 0;
            deployment.created_at_ms = now;
            deployment.updated_at_ms = now;

            ext_deployments_[deployment.command_id] = std::move(deployment);
            save_ext_deployments_locked();
        }

        std::ostringstream data;
        data << "{";
        data << "\"command_id\":\"" << json_escape(parsed.command.command_id) << "\",";
        data << "\"status\":\"pending\",";
        data << "\"target_count\":" << precheck.target_count << ",";
        data << "\"rollout_strategy\":{";
        data << "\"type\":\"" << json_escape(parsed.command.rollout.type) << "\",";
        data << "\"waves\":[";
        for (std::size_t i = 0; i < parsed.command.rollout.waves.size(); ++i) {
            if (i != 0) {
                data << ",";
            }
            data << parsed.command.rollout.waves[i];
        }
        data << "],";
        data << "\"rollback_on_failure\":" << bool_json(parsed.command.rollout.rollback_on_failure);
        data << "},";
        data << "\"run_at_utc\":";
        if (parsed.command.run_at_utc.has_value()) {
            data << *parsed.command.run_at_utc;
        } else {
            data << "null";
        }
        data << "}";

        return server::core::metrics::MetricsHttpServer::RouteResponse{
            "202 Accepted",
            "application/json; charset=utf-8",
            json_ok(request_id, data.str()).body};
    }

    server::core::metrics::MetricsHttpServer::RouteResponse
    handle_ext_deployments(std::uint64_t request_id,
                           const QueryOptions& options,
                           std::string_view query_string) {
        const auto params = parse_query_string(query_string);
        std::string command_id_filter;
        std::string status_filter;
        if (const auto it = params.find("command_id"); it != params.end()) {
            command_id_filter = trim_ascii(it->second);
        }
        if (const auto it = params.find("status"); it != params.end()) {
            status_filter = to_lower_ascii(trim_ascii(it->second));
        }

        std::vector<ExtDeploymentRecord> deployments;
        {
            std::lock_guard<std::mutex> lock(ext_mutex_);
            deployments.reserve(ext_deployments_.size());
            for (const auto& [_, deployment] : ext_deployments_) {
                deployments.push_back(deployment);
            }
        }

        std::sort(deployments.begin(), deployments.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.created_at_ms == rhs.created_at_ms) {
                return lhs.command_id < rhs.command_id;
            }
            return lhs.created_at_ms > rhs.created_at_ms;
        });

        std::vector<ExtDeploymentRecord> filtered;
        filtered.reserve(deployments.size());
        for (const auto& deployment : deployments) {
            if (!command_id_filter.empty() && deployment.command_id != command_id_filter) {
                continue;
            }
            if (!status_filter.empty() && to_lower_ascii(deployment.status) != status_filter) {
                continue;
            }
            filtered.push_back(deployment);
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
            if (cursor_index > filtered.size()) {
                http_errors_total_.fetch_add(1, std::memory_order_relaxed);
                return json_error(
                    request_id,
                    "400 Bad Request",
                    "BAD_REQUEST",
                    "cursor is out of range",
                    json_details("cursor", options.cursor));
            }
        }

        const std::size_t remaining = filtered.size() - cursor_index;
        const std::size_t take = std::min<std::size_t>(remaining, static_cast<std::size_t>(options.limit));
        const std::size_t end_index = cursor_index + take;

        std::ostringstream data;
        data << "{";
        data << "\"items\":[";
        for (std::size_t i = cursor_index; i < end_index; ++i) {
            const auto& deployment = filtered[i];
            if (i != cursor_index) {
                data << ",";
            }

            data << "{";
            data << "\"command_id\":\"" << json_escape(deployment.command_id) << "\",";
            data << "\"artifact_id\":\"" << json_escape(deployment.artifact_id) << "\",";
            data << "\"status\":\"" << json_escape(deployment.status) << "\",";
            data << "\"status_reason\":\"" << json_escape(deployment.status_reason) << "\",";
            data << "\"target_count\":" << deployment.target_count << ",";
            data << "\"applied_targets\":" << deployment.applied_targets << ",";
            data << "\"run_at_utc\":";
            if (deployment.run_at_utc.has_value()) {
                data << *deployment.run_at_utc;
            } else {
                data << "null";
            }
            data << ",";
            data << "\"rollout_strategy\":{";
            data << "\"type\":\"" << json_escape(deployment.rollout.type) << "\",";
            data << "\"waves\":[";
            for (std::size_t j = 0; j < deployment.rollout.waves.size(); ++j) {
                if (j != 0) {
                    data << ",";
                }
                data << deployment.rollout.waves[j];
            }
            data << "],";
            data << "\"rollback_on_failure\":" << bool_json(deployment.rollout.rollback_on_failure);
            data << "},";
            data << "\"issues\":[";
            for (std::size_t j = 0; j < deployment.issues.size(); ++j) {
                if (j != 0) {
                    data << ",";
                }
                data << "{";
                data << "\"code\":\"" << json_escape(deployment.issues[j].code) << "\",";
                data << "\"message\":\"" << json_escape(deployment.issues[j].message) << "\"";
                data << "}";
            }
            data << "],";
            data << "\"created_at_ms\":" << deployment.created_at_ms << ",";
            data << "\"updated_at_ms\":" << deployment.updated_at_ms;
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
        if (end_index < filtered.size()) {
            data << "\"" << end_index << "\"";
        } else {
            data << "null";
        }
        data << ",";
        data << "\"total\":" << filtered.size();
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

        std::optional<server::core::state::InstanceRecord> backend;
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

        std::unordered_map<std::string, server::core::state::InstanceRecord> instances;
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
        const SelectorParseResult selector_parse = parse_instance_selector_query(query_string);
        if (!selector_parse.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                selector_parse.error_message,
                "{" +
                    std::string("\"parameter\":\"") + json_escape(selector_parse.error_param) +
                    "\",\"value\":\"" + json_escape(selector_parse.error_value) + "\"}");
        }

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
        std::unordered_map<std::string, std::string> fields;
        fields["op"] = "disconnect";
        fields["actor"] = sanitize_single_line(auth.actor);
        fields["request_id"] = "admin-" + std::to_string(request_id);
        fields["reason"] = sanitize_single_line(reason);
        fields["client_ids"] = join_csv(deduped);
        append_selector_command_fields(fields, selector_parse);

        const auto message = build_signed_admin_message(std::move(fields), request_id);
        if (!message) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "MISCONFIGURED",
                "admin command signing is not configured");
        }

        if (!redis_->publish(channel, *message)) {
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
        data << "],";
        append_selector_response_json(data, selector_parse);
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
        const SelectorParseResult selector_parse = parse_instance_selector_query(query_string);
        if (!selector_parse.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                selector_parse.error_message,
                "{" +
                    std::string("\"parameter\":\"") + json_escape(selector_parse.error_param) +
                    "\",\"value\":\"" + json_escape(selector_parse.error_value) + "\"}");
        }

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
        std::unordered_map<std::string, std::string> fields;
        fields["op"] = "announce";
        fields["actor"] = sanitize_single_line(auth.actor);
        fields["request_id"] = "admin-" + std::to_string(request_id);
        fields["priority"] = priority;
        fields["text"] = text;
        append_selector_command_fields(fields, selector_parse);

        const auto message = build_signed_admin_message(std::move(fields), request_id);
        if (!message) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "MISCONFIGURED",
                "admin command signing is not configured");
        }

        if (!redis_->publish(channel, *message)) {
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
        data << "\"text\":\"" << json_escape(text) << "\",";
        append_selector_response_json(data, selector_parse);
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
        const SelectorParseResult selector_parse = parse_instance_selector_query(query_string);
        if (!selector_parse.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                selector_parse.error_message,
                "{" +
                    std::string("\"parameter\":\"") + json_escape(selector_parse.error_param) +
                    "\",\"value\":\"" + json_escape(selector_parse.error_value) + "\"}");
        }

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

        const auto* setting_rule = server::core::config::find_runtime_setting_rule(key);
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
        std::unordered_map<std::string, std::string> fields;
        fields["op"] = "settings";
        fields["actor"] = sanitize_single_line(auth.actor);
        fields["request_id"] = "admin-" + std::to_string(request_id);
        fields["key"] = sanitize_single_line(key);
        fields["value"] = std::to_string(parsed_value);
        append_selector_command_fields(fields, selector_parse);

        const auto message = build_signed_admin_message(std::move(fields), request_id);
        if (!message) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "MISCONFIGURED",
                "admin command signing is not configured");
        }

        if (!redis_->publish(channel, *message)) {
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
        data << "\"value\":" << parsed_value << ",";
        append_selector_response_json(data, selector_parse);
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
        const SelectorParseResult selector_parse = parse_instance_selector_query(query_string);
        if (!selector_parse.ok) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "400 Bad Request",
                "BAD_REQUEST",
                selector_parse.error_message,
                "{" +
                    std::string("\"parameter\":\"") + json_escape(selector_parse.error_param) +
                    "\",\"value\":\"" + json_escape(selector_parse.error_value) + "\"}");
        }

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
        std::unordered_map<std::string, std::string> fields;
        fields["op"] = sanitize_single_line(op);
        fields["actor"] = sanitize_single_line(auth.actor);
        fields["request_id"] = "admin-" + std::to_string(request_id);
        fields["duration_sec"] = std::to_string(duration_sec);
        fields["reason"] = reason;
        fields["client_ids"] = join_csv(deduped);
        append_selector_command_fields(fields, selector_parse);

        const auto message = build_signed_admin_message(std::move(fields), request_id);
        if (!message) {
            http_errors_total_.fetch_add(1, std::memory_order_relaxed);
            return json_error(
                request_id,
                "503 Service Unavailable",
                "MISCONFIGURED",
                "admin command signing is not configured");
        }

        if (!redis_->publish(channel, *message)) {
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
        data << "],";
        append_selector_response_json(data, selector_parse);
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
    bool admin_read_only_{false};
    std::string admin_command_signing_secret_;
    std::string ext_plugins_dir_;
    std::string ext_plugins_fallback_dir_;
    std::string ext_scripts_dir_;
    std::string ext_scripts_fallback_dir_;
    std::string ext_schedule_store_path_;
    std::uint32_t ext_max_clock_skew_ms_{kDefaultExtMaxClockSkewMs};
    std::uint32_t ext_force_fail_wave_index_{0};

    AdminAuthMode auth_mode_{AdminAuthMode::kOff};
    std::string auth_mode_raw_;
    std::string auth_off_role_;
    std::string auth_user_header_name_;
    std::string auth_role_header_name_;
    std::string auth_bearer_token_;
    std::string auth_bearer_actor_;
    std::string auth_bearer_role_;

    std::shared_ptr<server::core::storage::redis::IRedisClient> redis_;
    std::shared_ptr<server::core::state::IInstanceStateBackend> registry_backend_;

    std::thread poller_;
    std::atomic<bool> poller_running_{false};

    mutable std::mutex cache_mutex_;
    mutable std::mutex ext_mutex_;
    std::vector<server::core::state::InstanceRecord> instances_cache_;
    std::unordered_map<std::string, server::core::state::InstanceRecord> instances_index_;
    std::unordered_map<std::string, InstanceDetailSnapshot> instance_details_index_;
    std::uint64_t instances_updated_at_ms_{0};
    WorkerSnapshot worker_cache_;
    std::deque<AuditTrendPoint> audit_trend_points_;
    std::vector<ExtArtifactInventoryItem> ext_inventory_cache_;
    std::unordered_map<std::string, std::size_t> ext_inventory_index_;
    std::unordered_map<std::string, ExtPrecheckRecord> ext_precheck_cache_;
    std::unordered_map<std::string, ExtDeploymentRecord> ext_deployments_;
    std::uint64_t ext_inventory_updated_at_ms_{0};

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
    std::atomic<std::uint64_t> instances_selector_requests_total_{0};
    std::atomic<std::uint64_t> instances_selector_mismatch_total_{0};
    std::atomic<std::uint64_t> session_lookup_requests_total_{0};
    std::atomic<std::uint64_t> users_requests_total_{0};
    std::atomic<std::uint64_t> disconnect_requests_total_{0};
    std::atomic<std::uint64_t> announce_requests_total_{0};
    std::atomic<std::uint64_t> settings_requests_total_{0};
    std::atomic<std::uint64_t> moderation_requests_total_{0};
    std::atomic<std::uint64_t> worker_requests_total_{0};
    std::atomic<std::uint64_t> poll_errors_total_{0};
    std::atomic<std::uint64_t> command_signing_errors_total_{0};
    std::atomic<std::uint64_t> ext_inventory_requests_total_{0};
    std::atomic<std::uint64_t> ext_precheck_requests_total_{0};
    std::atomic<std::uint64_t> ext_deployments_requests_total_{0};
    std::atomic<std::uint64_t> ext_schedules_requests_total_{0};
    std::atomic<std::uint64_t> ext_precheck_failed_total_{0};
    std::atomic<std::uint64_t> ext_command_id_conflict_total_{0};
    std::atomic<std::uint64_t> ext_scheduler_runs_total_{0};
    std::atomic<std::uint64_t> ext_scheduler_due_total_{0};
    std::atomic<std::uint64_t> ext_deployment_wave_total_{0};
    std::atomic<std::uint64_t> ext_wave_failed_total_{0};
    std::atomic<std::uint64_t> ext_rollbacks_total_{0};
    std::atomic<std::uint64_t> ext_clock_skew_failed_total_{0};
    std::atomic<std::uint64_t> ext_deployments_completed_total_{0};
    std::atomic<std::uint64_t> ext_deployments_failed_total_{0};
    std::atomic<std::uint64_t> ext_store_read_ok_total_{0};
    std::atomic<std::uint64_t> ext_store_read_fail_total_{0};
    std::atomic<std::uint64_t> ext_store_write_ok_total_{0};
    std::atomic<std::uint64_t> ext_inventory_error_count_{0};
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
