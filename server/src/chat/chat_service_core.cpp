#include "server/chat/chat_service.hpp"
#include "server/app/topology_runtime_assignment.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/config/runtime_settings.hpp"
#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/trace/context.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/service_registry.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/scripting/chat_lua_bindings.hpp"
#include "server/wire/codec.hpp"
#include "chat_hook_plugin_chain.hpp"
#include "wire.pb.h"
// 저장소 연동 헤더
#include "server/storage/connection_pool.hpp"
#include "server/core/storage/redis/client.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <random>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>
#include <unordered_set>
using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;
namespace services = server::core::util::services;

/**
 * @brief ChatService 코어 상태, 설정, 플러그인 초기화 구현입니다.
 *
 * DB, Redis, write-behind, 프레즌스, 히스토리 설정을 한곳에서 해석해,
 * 핸들러 로직이 환경별 분기 없이 동일 인터페이스를 사용하도록 만듭니다.
 */
namespace server::app::chat {

struct ChatService::HookPluginState {
    explicit HookPluginState(ChatHookPluginChain::Config cfg)
        : chain(std::move(cfg)) {}
    ChatHookPluginChain chain;
};

namespace {

constexpr std::string_view kRoomPasswordHashPrefix = "sha256:";
constexpr std::string_view kAppMigrationPayloadRoomKind = "chat-room-v1";

std::string legacy_hash_room_password(std::string_view password) {
    std::hash<std::string> hasher;
    const std::size_t value = hasher(std::string(password));
    std::ostringstream oss;
    oss << std::hex << value;
    return oss.str();
}

std::string sha256_hex(std::string_view input) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data()) == nullptr) {
        return {};
    }

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        out.push_back(kHexDigits[(byte >> 4) & 0x0F]);
        out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
}

bool has_room_password_hash_prefix(std::string_view value) {
    return value.rfind(kRoomPasswordHashPrefix, 0) == 0;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(static_cast<char>(ch));
            break;
        }
    }
    return out;
}

std::string json_array_of_strings(const std::vector<std::string>& values) {
    std::string out = "[";
    bool first = true;
    for (const auto& value : values) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += '"';
        out += json_escape(value);
        out += '"';
    }
    out += ']';
    return out;
}

std::string lua_session_event_name(SessionEventKindV2 kind) {
    switch (kind) {
    case SessionEventKindV2::kOpen:
        return "open";
    case SessionEventKindV2::kClose:
        return "close";
    default:
        return "unknown";
    }
}

std::optional<std::string> extract_world_tag(std::string_view value) {
    static constexpr std::string_view kWorldPrefix = "world:";
    if (value.rfind(kWorldPrefix, 0) != 0 || value.size() <= kWorldPrefix.size()) {
        return std::nullopt;
    }
    return std::string(value.substr(kWorldPrefix.size()));
}

} // namespace

// 생성자에서 환경별 설정과 선택 의존성을 모두 정리해 두면,
// 이후 핸들러는 "무엇이 켜져 있는가"를 매번 다시 해석하지 않고 공용 상태만 보면 된다.
ChatService::ChatService(boost::asio::io_context& io,
                         server::core::JobQueue& job_queue,
    std::shared_ptr<server::storage::IRepositoryConnectionPool> db_pool,
    std::shared_ptr<server::core::storage::redis::IRedisClient> redis)
    : io_(&io), job_queue_(job_queue), db_pool_(std::move(db_pool)), redis_(std::move(redis)) {

    // 명시 주입이 없으면 레지스트리 폴백(fallback)을 사용한다.
    // 부트스트랩은 간단해지지만, 어떤 의존성이 빠졌는지 추적 가능한 seam은 유지한다.
    if (!db_pool_) {
        db_pool_ = services::get<server::storage::IRepositoryConnectionPool>();
    }
    if (!redis_) {
        redis_ = services::get<server::core::storage::redis::IRedisClient>();
    }
    lua_runtime_ = services::get<server::core::scripting::LuaRuntime>();
    lua_execution_strand_ = services::get<Strand>();
    if (!lua_execution_strand_ && lua_runtime_ && io_) {
        lua_execution_strand_ = std::make_shared<Strand>(boost::asio::make_strand(*io_));
    }
    if (lua_runtime_) {
        const auto bindings = server::app::scripting::register_chat_lua_bindings(*lua_runtime_, *this);
        corelog::info("Lua host bindings initialised attempted="
                      + std::to_string(bindings.attempted)
                      + " registered=" + std::to_string(bindings.registered));
    }

    // 게이트웨이 ID는 분산 전파(fan-out) 루프를 식별하는 최소 표식이다.
    // 이 값이 없으면 자기 자신이 보낸 메시지를 다시 받은 상황을 추적하기 어려워진다.
    if (const char* gw = std::getenv("GATEWAY_ID"); gw && *gw) {
        gateway_id_ = gw;
    }

    // write-behind는 요청 지연시간과 영속화 내구성을 분리하는 선택지다.
    // 서버 요청 경로가 DB 지연에 직접 매달리지 않게 하되, 스트림 누락 여부는 별도 worker가 감시한다.
    if (const char* flag = std::getenv("WRITE_BEHIND_ENABLED"); flag && *flag && std::string(flag) != "0") {
        write_behind_.enabled = true;
    }
    if (const char* key = std::getenv("REDIS_STREAM_KEY"); key && *key) {
        write_behind_.stream_key = key;
    }
    if (const char* maxlen = std::getenv("REDIS_STREAM_MAXLEN"); maxlen && *maxlen) {
        char* end = nullptr;
        unsigned long long value = std::strtoull(maxlen, &end, 10);
        if (end != maxlen && value > 0) {
            write_behind_.maxlen = static_cast<std::size_t>(value);
        }
    }
    if (const char* approx = std::getenv("REDIS_STREAM_APPROX"); approx && *approx) {
        if (std::string(approx) == "0") {
            write_behind_.approximate = false;
        }
    }

    // 프레즌스 TTL은 "살아 있음"을 추정하는 운영 규칙이다.
    // 너무 짧으면 정상 세션도 쉽게 offline으로 보이고, 너무 길면 끊긴 사용자가 오래 남는다.
    if (const char* ttl = std::getenv("PRESENCE_TTL_SEC"); ttl && *ttl) {
        unsigned long t = std::strtoul(ttl, nullptr, 10);
        if (t > 0 && t < 3600) {
            presence_.ttl = static_cast<unsigned int>(t);
        }
    }
    if (const char* prefix = std::getenv("REDIS_CHANNEL_PREFIX"); prefix && *prefix) {
        presence_.prefix = prefix;
    }

    if (const char* enabled = std::getenv("SESSION_CONTINUITY_ENABLED"); enabled && *enabled) {
        continuity_.enabled = (std::strcmp(enabled, "0") != 0);
    }
    if (const char* ttl = std::getenv("SESSION_CONTINUITY_LEASE_TTL_SEC"); ttl && *ttl) {
        unsigned long value = std::strtoul(ttl, nullptr, 10);
        if (value >= 30 && value <= 7 * 24 * 60 * 60) {
            continuity_.lease_ttl_sec = static_cast<unsigned int>(value);
        }
    }
    if (const char* prefix = std::getenv("SESSION_CONTINUITY_REDIS_PREFIX"); prefix && *prefix) {
        continuity_.redis_prefix = prefix;
    } else {
        continuity_.redis_prefix = presence_.prefix;
    }
    if (!continuity_.redis_prefix.empty() && continuity_.redis_prefix.back() != ':') {
        continuity_.redis_prefix.push_back(':');
    }
    continuity_.redis_prefix += "continuity:";

    if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
        redis_pubsub_enabled_ = true;
    }

    // 환경 변수 읽기 헬퍼 함수
    const auto read_env = [](const char* primary, const char* secondary = nullptr) -> const char* {
        if (primary) {
            if (const char* value = std::getenv(primary); value && *value) {
                return value;
            }
        }
        if (secondary) {
            if (const char* value = std::getenv(secondary); value && *value) {
                return value;
            }
        }
        return nullptr;
    };

    const auto split_csv = [](std::string_view raw) {
        std::vector<std::string> out;
        std::string current;
        auto flush = [&]() {
            std::size_t begin = 0;
            while (begin < current.size() && std::isspace(static_cast<unsigned char>(current[begin])) != 0) {
                ++begin;
            }
            std::size_t end = current.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(current[end - 1])) != 0) {
                --end;
            }
            if (end > begin) {
                out.emplace_back(current.substr(begin, end - begin));
            }
            current.clear();
        };
        for (char c : raw) {
            if (c == ',' || c == ';') {
                flush();
                continue;
            }
            current.push_back(c);
        }
        flush();
        return out;
    };

    if (const char* world_default = std::getenv("WORLD_ADMISSION_DEFAULT"); world_default && *world_default) {
        continuity_.default_world_id = world_default;
    } else if (const char* tags_env = std::getenv("SERVER_TAGS"); tags_env && *tags_env) {
        for (const auto& tag : split_csv(tags_env)) {
            if (auto world_id = extract_world_tag(tag); world_id.has_value()) {
                continuity_.default_world_id = *world_id;
                break;
            }
        }
    }
    if (const char* owner_id = std::getenv("SERVER_INSTANCE_ID"); owner_id && *owner_id) {
        continuity_.current_owner_id = owner_id;
    } else if (const char* owner_id = std::getenv("GATEWAY_ID"); owner_id && *owner_id) {
        continuity_.current_owner_id = owner_id;
    } else {
        continuity_.current_owner_id = "server-owner-" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    if (const char* assignment_key = std::getenv("TOPOLOGY_RUNTIME_ASSIGNMENT_KEY");
        assignment_key && *assignment_key) {
        continuity_.topology_runtime_assignment_key = assignment_key;
    } else {
        continuity_.topology_runtime_assignment_key = "dynaxis:topology:actuation:runtime-assignment";
    }

    if (const char* admins_env = read_env("CHAT_ADMIN_USERS", "ADMIN_USERS"); admins_env && *admins_env) {
        for (const auto& user : split_csv(admins_env)) {
            admin_users_.insert(user);
        }
    }

    const auto parse_u32_bounded = [](const char* raw,
                                      std::uint32_t fallback,
                                      std::uint32_t min_value,
                                      std::uint32_t max_value) {
        if (!raw || !*raw) {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(raw, &end, 10);
        if (end == raw || parsed < min_value || parsed > max_value) {
            return fallback;
        }
        return static_cast<std::uint32_t>(parsed);
    };

    spam_message_threshold_ = parse_u32_bounded(std::getenv("CHAT_SPAM_THRESHOLD"), 6, 3, 100);
    spam_window_sec_ = parse_u32_bounded(std::getenv("CHAT_SPAM_WINDOW_SEC"), 5, 1, 120);
    spam_mute_sec_ = parse_u32_bounded(std::getenv("CHAT_SPAM_MUTE_SEC"), 30, 5, 86400);
    spam_ban_sec_ = parse_u32_bounded(std::getenv("CHAT_SPAM_BAN_SEC"), 600, 10, 604800);
    spam_ban_violation_threshold_ = parse_u32_bounded(std::getenv("CHAT_SPAM_BAN_VIOLATIONS"), 3, 1, 20);
    lua_auto_disable_threshold_ = parse_u32_bounded(std::getenv("LUA_AUTO_DISABLE_THRESHOLD"), 3, 1, 1'000'000);
    lua_hook_warn_budget_us_ = parse_u32_bounded(std::getenv("LUA_HOOK_WARN_BUDGET_US"), 0, 0, 60'000'000);
    const std::uint32_t chat_hook_warn_budget_us = parse_u32_bounded(
        std::getenv("CHAT_HOOK_WARN_BUDGET_US"),
        0,
        0,
        60'000'000);

    // 최근 대화 내역(History) 관련 설정
    if (const char* limit_env = read_env("RECENT_HISTORY_LIMIT", "SNAPSHOT_RECENT_LIMIT")) {
        char* end = nullptr;
        unsigned long value = std::strtoul(limit_env, &end, 10);
        if (limit_env != end && value >= 5 && value <= 2000) {
            history_.recent_limit = static_cast<std::size_t>(value);
        }
    }
    if (const char* maxlen_env = std::getenv("ROOM_RECENT_MAXLEN"); maxlen_env && *maxlen_env) {
        char* end = nullptr;
        unsigned long value = std::strtoul(maxlen_env, &end, 10);
        if (maxlen_env != end && value >= history_.recent_limit && value <= 5000) {
            history_.max_list_len = static_cast<std::size_t>(value);
        }
    }
    if (const char* ttl_env = std::getenv("CACHE_TTL_RECENT_MSGS"); ttl_env && *ttl_env) {
        char* end = nullptr;
        unsigned long value = std::strtoul(ttl_env, &end, 10);
        if (ttl_env != end && value >= 60 && value <= 604800) {
            history_.cache_ttl_sec = static_cast<unsigned int>(value);
        }
    }
    if (const char* fetch_env = read_env("RECENT_HISTORY_FETCH_FACTOR", "SNAPSHOT_FETCH_FACTOR")) {
        char* end = nullptr;
        unsigned long value = std::strtoul(fetch_env, &end, 10);
        if (fetch_env != end && value >= 1 && value <= 10) {
            history_.fetch_factor = static_cast<std::size_t>(value);
        }
    }
    if (history_.max_list_len < history_.recent_limit) {
        history_.max_list_len = history_.recent_limit;
    }

    if (write_behind_.enabled) {
        corelog::info(std::string("Write-behind enabled: stream=") + write_behind_.stream_key +
                      (write_behind_.maxlen ? (std::string(", maxlen=") + std::to_string(*write_behind_.maxlen)) : std::string(", maxlen=none")) +
                      std::string(write_behind_.approximate ? ", approx=~" : ", approx=exact"));
    } else {
        corelog::warn("Write-behind disabled (set WRITE_BEHIND_ENABLED=1 to enable)");
    }

    // Optional chat-hook plugins (hot-reloadable shared libraries)
    {
        ChatHookPluginChain::Config cfg;

        const auto split_paths = [](const char* raw) -> std::vector<std::filesystem::path> {
            std::vector<std::filesystem::path> out;
            if (!raw || !*raw) {
                return out;
            }

            auto trim = [](std::string& s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
                    s.erase(s.begin());
                }
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
                    s.pop_back();
                }
            };

            std::string cur;
            for (const char* p = raw; *p; ++p) {
                const char c = *p;
                if (c == ';' || c == ',') {
                    trim(cur);
                    if (!cur.empty()) {
                        out.emplace_back(cur);
                    }
                    cur.clear();
                    continue;
                }
                cur.push_back(c);
            }
            trim(cur);
            if (!cur.empty()) {
                out.emplace_back(cur);
            }
            return out;
        };

        bool configured = false;
        bool plugins_dir_from_env = false;
        if (const char* list = std::getenv("CHAT_HOOK_PLUGIN_PATHS"); list && *list) {
            cfg.plugin_paths = split_paths(list);
            configured = !cfg.plugin_paths.empty();
        }

        if (!configured) {
            if (const char* dir = std::getenv("CHAT_HOOK_PLUGINS_DIR"); dir && *dir) {
                cfg.plugins_dir = std::filesystem::path(dir);
                configured = true;
                plugins_dir_from_env = true;
            }
        }

        if (!configured) {
            if (const char* val = std::getenv("CHAT_HOOK_PLUGIN_PATH"); val && *val) {
                cfg.plugin_paths.emplace_back(val);
                configured = true;
            }
        }

        bool chat_hook_enabled = true;
        bool chat_hook_enabled_overridden = false;
        if (const char* hook_enabled = std::getenv("CHAT_HOOK_ENABLED"); hook_enabled && *hook_enabled) {
            chat_hook_enabled_overridden = true;
            chat_hook_enabled = (std::strcmp(hook_enabled, "0") != 0);
        }

        if (!chat_hook_enabled && configured) {
            corelog::info("CHAT_HOOK_ENABLED=0; chat hook plugins are configured but runtime path is disabled");
        }
        if (chat_hook_enabled_overridden && chat_hook_enabled && !configured) {
            corelog::warn("CHAT_HOOK_ENABLED=1 but no plugin path/directory is configured");
        }

        const bool enabled = configured && chat_hook_enabled;

        if (enabled && plugins_dir_from_env) {
            if (const char* fallback = std::getenv("CHAT_HOOK_FALLBACK_PLUGINS_DIR"); fallback && *fallback) {
                cfg.fallback_plugins_dir = std::filesystem::path(fallback);
            }
        }

        if (enabled) {
            cfg.hook_warn_budget_us = chat_hook_warn_budget_us;
            if (const char* cache = std::getenv("CHAT_HOOK_CACHE_DIR"); cache && *cache) {
                cfg.cache_dir = cache;
            }
            if (const char* lock = std::getenv("CHAT_HOOK_LOCK_PATH"); lock && *lock) {
                cfg.single_lock_path = lock;
            }

            hook_plugin_ = std::make_unique<HookPluginState>(std::move(cfg));
            hook_plugin_->chain.poll_reload();

            unsigned long interval_ms = 500;
            if (const char* interval = std::getenv("CHAT_HOOK_RELOAD_INTERVAL_MS"); interval && *interval) {
                interval_ms = std::strtoul(interval, nullptr, 10);
            }
            if (interval_ms > 0) {
                if (auto scheduler = services::get<server::core::concurrent::TaskScheduler>()) {
                    scheduler->schedule_every([this]() {
                        if (!hook_plugin_) {
                            return;
                        }
                        (void)job_queue_.TryPush([this]() {
                            if (hook_plugin_) {
                                hook_plugin_->chain.poll_reload();
                            }
                        });
                    }, std::chrono::milliseconds{static_cast<long long>(interval_ms)});
                }
            }
        }
    }
}

ChatService::~ChatService() = default;

ChatService::ChatHookPluginsMetrics ChatService::chat_hook_plugins_metrics() const {
    ChatHookPluginsMetrics out{};
    if (!hook_plugin_) {
        out.enabled = false;
        out.mode = "none";
        return out;
    }

    const auto snap = hook_plugin_->chain.metrics_snapshot();
    out.enabled = snap.configured;
    out.mode = snap.mode;
    out.plugins.reserve(snap.plugins.size());
    for (const auto& p : snap.plugins) {
        ChatHookPluginMetric m{};
        m.file = p.plugin_path.filename().string();
        m.loaded = p.loaded;
        m.name = p.name;
        m.version = p.version;
        m.reload_attempt_total = p.reload_attempt_total;
        m.reload_success_total = p.reload_success_total;
        m.reload_failure_total = p.reload_failure_total;
        m.hook_metrics.reserve(p.hook_metrics.size());
        for (const auto& hm : p.hook_metrics) {
            ChatHookPluginMetric::HookMetric metric{};
            metric.hook_name = hm.hook_name;
            metric.calls_total = hm.calls_total;
            metric.errors_total = hm.errors_total;
            metric.duration_count = hm.duration_count;
            metric.duration_sum_ns = hm.duration_sum_ns;
            metric.duration_bucket_counts = hm.duration_bucket_counts;
            m.hook_metrics.push_back(std::move(metric));
        }
        out.plugins.push_back(std::move(m));
    }
    return out;
}

ChatService::LuaHooksMetrics ChatService::lua_hooks_metrics() const {
    LuaHooksMetrics out{};
    out.enabled = static_cast<bool>(lua_runtime_);
    out.auto_disable_threshold = lua_auto_disable_threshold_;

    if (!lua_runtime_) {
        return out;
    }

    const auto runtime_snapshot = lua_runtime_->metrics_snapshot();
    out.reload_epoch = runtime_snapshot.reload_epoch;
    out.loaded_scripts = runtime_snapshot.loaded_scripts;
    out.memory_used_bytes = runtime_snapshot.memory_used_bytes;
    out.calls_total = runtime_snapshot.calls_total;
    out.errors_total = runtime_snapshot.errors_total;
    out.instruction_limit_hits = runtime_snapshot.instruction_limit_hits;
    out.memory_limit_hits = runtime_snapshot.memory_limit_hits;

    std::lock_guard<std::mutex> lock(lua_hook_metrics_mu_);
    out.hooks.reserve(
        lua_hook_consecutive_failures_.size()
        + lua_hook_auto_disable_total_.size()
        + lua_hook_calls_total_.size()
        + lua_hook_errors_total_.size()
        + lua_hook_instruction_limit_hits_.size()
        + lua_hook_memory_limit_hits_.size()
        + lua_hook_disabled_.size());

    auto append_or_get = [&](const std::string& hook_name) -> LuaHookMetric& {
        for (auto& metric : out.hooks) {
            if (metric.hook_name == hook_name) {
                return metric;
            }
        }
        out.hooks.push_back(LuaHookMetric{hook_name, false, 0, 0});
        return out.hooks.back();
    };

    for (const auto& [hook_name, failures] : lua_hook_consecutive_failures_) {
        auto& metric = append_or_get(hook_name);
        metric.consecutive_failures = failures;
    }

    for (const auto& [hook_name, total] : lua_hook_auto_disable_total_) {
        auto& metric = append_or_get(hook_name);
        metric.auto_disable_total = total;
    }

    for (const auto& [hook_name, total] : lua_hook_calls_total_) {
        auto& metric = append_or_get(hook_name);
        metric.calls_total = total;
    }

    for (const auto& [hook_name, total] : lua_hook_errors_total_) {
        auto& metric = append_or_get(hook_name);
        metric.errors_total = total;
    }

    for (const auto& [hook_name, total] : lua_hook_instruction_limit_hits_) {
        auto& metric = append_or_get(hook_name);
        metric.instruction_limit_hits = total;
    }

    for (const auto& [hook_name, total] : lua_hook_memory_limit_hits_) {
        auto& metric = append_or_get(hook_name);
        metric.memory_limit_hits = total;
    }

    for (const auto& hook_name : lua_hook_disabled_) {
        auto& metric = append_or_get(hook_name);
        metric.disabled = true;
    }

    auto append_or_get_script_metric = [&](const std::string& hook_name,
                                           const std::string& script_name) -> LuaScriptCallMetric& {
        for (auto& metric : out.script_calls) {
            if (metric.hook_name == hook_name && metric.script_name == script_name) {
                return metric;
            }
        }
        out.script_calls.push_back(LuaScriptCallMetric{hook_name, script_name, 0, 0});
        return out.script_calls.back();
    };

    for (const auto& [hook_name, script_map] : lua_hook_script_calls_total_) {
        for (const auto& [script_name, total] : script_map) {
            auto& metric = append_or_get_script_metric(hook_name, script_name);
            metric.calls_total = total;
        }
    }

    for (const auto& [hook_name, script_map] : lua_hook_script_errors_total_) {
        for (const auto& [script_name, total] : script_map) {
            auto& metric = append_or_get_script_metric(hook_name, script_name);
            metric.errors_total = total;
        }
    }

    std::sort(out.hooks.begin(), out.hooks.end(), [](const LuaHookMetric& lhs, const LuaHookMetric& rhs) {
        return lhs.hook_name < rhs.hook_name;
    });

    std::sort(out.script_calls.begin(), out.script_calls.end(), [](const LuaScriptCallMetric& lhs, const LuaScriptCallMetric& rhs) {
        if (lhs.hook_name == rhs.hook_name) {
            return lhs.script_name < rhs.script_name;
        }
        return lhs.hook_name < rhs.hook_name;
    });

    return out;
}

ChatService::ContinuityMetrics ChatService::continuity_metrics() const {
    ContinuityMetrics out{};
    out.lease_issue_total = continuity_lease_issue_total_.load(std::memory_order_relaxed);
    out.lease_issue_fail_total = continuity_lease_issue_fail_total_.load(std::memory_order_relaxed);
    out.lease_resume_total = continuity_lease_resume_total_.load(std::memory_order_relaxed);
    out.lease_resume_fail_total = continuity_lease_resume_fail_total_.load(std::memory_order_relaxed);
    out.state_write_total = continuity_state_write_total_.load(std::memory_order_relaxed);
    out.state_write_fail_total = continuity_state_write_fail_total_.load(std::memory_order_relaxed);
    out.state_restore_total = continuity_state_restore_total_.load(std::memory_order_relaxed);
    out.state_restore_fallback_total = continuity_state_restore_fallback_total_.load(std::memory_order_relaxed);
    out.world_write_total = continuity_world_write_total_.load(std::memory_order_relaxed);
    out.world_write_fail_total = continuity_world_write_fail_total_.load(std::memory_order_relaxed);
    out.world_restore_total = continuity_world_restore_total_.load(std::memory_order_relaxed);
    out.world_restore_fallback_total = continuity_world_restore_fallback_total_.load(std::memory_order_relaxed);
    out.world_restore_fallback_missing_world_total =
        continuity_world_restore_fallback_missing_world_total_.load(std::memory_order_relaxed);
    out.world_restore_fallback_missing_owner_total =
        continuity_world_restore_fallback_missing_owner_total_.load(std::memory_order_relaxed);
    out.world_restore_fallback_owner_mismatch_total =
        continuity_world_restore_fallback_owner_mismatch_total_.load(std::memory_order_relaxed);
    out.world_restore_fallback_draining_replacement_unhonored_total =
        continuity_world_restore_fallback_draining_replacement_unhonored_total_.load(std::memory_order_relaxed);
    out.world_owner_write_total = continuity_world_owner_write_total_.load(std::memory_order_relaxed);
    out.world_owner_write_fail_total = continuity_world_owner_write_fail_total_.load(std::memory_order_relaxed);
    out.world_owner_restore_total = continuity_world_owner_restore_total_.load(std::memory_order_relaxed);
    out.world_owner_restore_fallback_total = continuity_world_owner_restore_fallback_total_.load(std::memory_order_relaxed);
    out.world_migration_restore_total = continuity_world_migration_restore_total_.load(std::memory_order_relaxed);
    out.world_migration_restore_fallback_total =
        continuity_world_migration_restore_fallback_total_.load(std::memory_order_relaxed);
    out.world_migration_restore_fallback_target_world_missing_total =
        continuity_world_migration_restore_fallback_target_world_missing_total_.load(std::memory_order_relaxed);
    out.world_migration_restore_fallback_target_owner_missing_total =
        continuity_world_migration_restore_fallback_target_owner_missing_total_.load(std::memory_order_relaxed);
    out.world_migration_restore_fallback_target_owner_not_ready_total =
        continuity_world_migration_restore_fallback_target_owner_not_ready_total_.load(std::memory_order_relaxed);
    out.world_migration_restore_fallback_target_owner_mismatch_total =
        continuity_world_migration_restore_fallback_target_owner_mismatch_total_.load(std::memory_order_relaxed);
    out.world_migration_restore_fallback_source_not_draining_total =
        continuity_world_migration_restore_fallback_source_not_draining_total_.load(std::memory_order_relaxed);
    out.world_migration_payload_room_handoff_total =
        continuity_world_migration_payload_room_handoff_total_.load(std::memory_order_relaxed);
    out.world_migration_payload_room_handoff_fallback_total =
        continuity_world_migration_payload_room_handoff_fallback_total_.load(std::memory_order_relaxed);
    return out;
}

// 방별 Strand(직렬화된 실행 컨텍스트)를 반환합니다.
// 동일한 방에 대한 작업은 동일한 스레드에서 순차적으로 실행됨을 보장합니다.
// 이는 멀티스레드 환경에서 방 상태(참여자 목록 등)의 동시성 문제를 해결하는 핵심 메커니즘입니다.
// Mutex를 직접 사용하는 것보다 데드락 위험이 적고 코드가 간결해집니다.
ChatService::Strand& ChatService::strand_for(const std::string& room) {
    auto it = room_strands_.find(room);
    if (it == room_strands_.end()) {
        it = room_strands_.emplace(room, std::make_shared<Strand>(io_->get_executor())).first;
    }
    return *it->second;
}

bool ChatService::write_behind_enabled() const {
    return write_behind_.enabled && static_cast<bool>(redis_);
}

bool ChatService::pubsub_enabled() {
    return redis_pubsub_enabled_;
}

// UUID v4 생성 (난수 기반)
std::string ChatService::generate_uuid_v4() {
    std::array<unsigned char, 16> b{};
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (size_t i = 0; i < b.size(); i += 8) {
        auto v = rng();
        for (int j = 0; j < 8 && (i + j) < b.size(); ++j) b[i + j] = static_cast<unsigned char>((v >> (j * 8)) & 0xFF);
    }
    // RFC 4122 variant & version
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40); // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80); // variant 10xx
    auto hex = [](unsigned char c) { const char* d = "0123456789abcdef"; return std::pair<char,char>{d[(c>>4)&0xF], d[c&0xF]}; };
    std::string s; s.resize(36);
    int k = 0;
    for (int i = 0; i < 16; ++i) {
        auto [h,l] = hex(b[i]); s[k++] = h; s[k++] = l;
        if (i==3 || i==5 || i==7 || i==9) s[k++] = '-';
    }
    return s;
}

// 세션별 고유 UUID를 조회하거나 생성합니다.
std::string ChatService::get_or_create_session_uuid(Session& s) {
    std::lock_guard<std::mutex> lk(state_.mu);
    auto it = state_.session_uuid.find(&s);
    if (it != state_.session_uuid.end() && !it->second.empty()) return it->second;
    std::string id = generate_uuid_v4();
    state_.session_uuid[&s] = id;
    return id;
}

// write-behind 이벤트는 "지금 응답해야 하는 요청"과 "나중에 재시도 가능한 영속화"를 분리하는 seam이다.
// 별도 worker가 스트림을 읽어 적재하므로, 핫패스 응답을 느린 DB와 직접 묶지 않게 된다.
void ChatService::emit_write_behind_event(const std::string& type,
                                           const std::string& session_id,
                                           const std::optional<std::string>& user_id,
                                           const std::optional<std::string>& room_id,
                                           std::vector<std::pair<std::string, std::string>> extra_fields) {
    if (!write_behind_enabled() || type.empty() || session_id.empty()) {
        return;
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(6 + extra_fields.size());
    fields.emplace_back("type", type);
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fields.emplace_back("ts_ms", std::to_string(now_ms));
    fields.emplace_back("session_id", session_id);
    if (user_id && !user_id->empty()) {
        fields.emplace_back("user_id", *user_id);
    }
    if (room_id && !room_id->empty()) {
        fields.emplace_back("room_id", *room_id);
    }
    if (!gateway_id_.empty()) {
        fields.emplace_back("gateway_id", gateway_id_);
    }

    if (const auto trace_id = server::core::trace::current_trace_id(); !trace_id.empty()) {
        fields.emplace_back("trace_id", trace_id);
    }
    if (const auto correlation_id = server::core::trace::current_correlation_id(); !correlation_id.empty()) {
        fields.emplace_back("correlation_id", correlation_id);
    }

    for (auto& kv : extra_fields) {
        if (!kv.first.empty() && !kv.second.empty()) {
            fields.emplace_back(std::move(kv));
        }
    }
    // XADD는 실시간 전파용 PUBLISH와 달리 재시도 가능한 적재 경로를 만든다.
    // 영속 저장소가 잠시 느리거나 실패해도 worker가 다시 읽을 수 있어, 채팅 경로 꼬리 지연시간(tail latency)을 안정화한다.
    if (server::core::trace::current_sampled()) {
        corelog::debug("span_start component=server span=redis_xadd");
    }

    const bool xadd_ok = redis_->xadd(write_behind_.stream_key, fields, nullptr, write_behind_.maxlen, write_behind_.approximate);

    if (server::core::trace::current_sampled()) {
        corelog::debug(std::string("span_end component=server span=redis_xadd success=") + (xadd_ok ? "true" : "false"));
    }

    if (!xadd_ok) {
        corelog::warn(std::string("write-behind XADD failed: type=") + type);
    }
}

// WeakPtr로 관리되는 세션 목록에서 유효한 세션만 수집합니다.
void ChatService::collect_room_sessions(RoomSet& set, std::vector<std::shared_ptr<Session>>& out) {
    for (auto it = set.begin(); it != set.end(); ) {
        if (auto session_sp = it->lock()) {
            out.emplace_back(std::move(session_sp));
            ++it;
        } else {
            it = set.erase(it);
        }
    }
}

unsigned int ChatService::presence_ttl() const {
    return presence_.ttl;
}

std::string ChatService::make_presence_key(std::string_view category, const std::string& id) const {
    std::string key;
    key.reserve(presence_.prefix.size() + category.size() + id.size());
    key.append(presence_.prefix);
    key.append(category);
    key.append(id);
    return key;
}

std::string ChatService::make_continuity_room_key(const std::string& logical_session_id) const {
    return continuity_.redis_prefix + "room:" + logical_session_id;
}

std::string ChatService::make_continuity_world_key(const std::string& logical_session_id) const {
    return continuity_.redis_prefix + "world:" + logical_session_id;
}

std::string ChatService::make_continuity_world_owner_key(const std::string& world_id) const {
    return continuity_.redis_prefix + "world-owner:" + world_id;
}

std::string ChatService::make_continuity_world_policy_key(const std::string& world_id) const {
    return continuity_.redis_prefix + "world-policy:" + world_id;
}

std::string ChatService::make_continuity_world_migration_key(const std::string& world_id) const {
    return continuity_.redis_prefix + "world-migration:" + world_id;
}

bool ChatService::continuity_enabled() const {
    return continuity_.enabled && static_cast<bool>(db_pool_);
}

std::optional<std::string> ChatService::extract_resume_token(std::string_view token) const {
    static constexpr std::string_view kResumePrefix = "resume:";
    if (token.rfind(kResumePrefix, 0) != 0) {
        return std::nullopt;
    }

    token.remove_prefix(kResumePrefix.size());
    if (token.empty()) {
        return std::nullopt;
    }
    return std::string(token);
}

std::optional<std::string> ChatService::load_continuity_room(const std::string& logical_session_id) {
    if (!redis_ || logical_session_id.empty()) {
        return std::nullopt;
    }
    return redis_->get(make_continuity_room_key(logical_session_id));
}

std::optional<std::string> ChatService::load_continuity_world(const std::string& logical_session_id) {
    if (!redis_ || logical_session_id.empty()) {
        return std::nullopt;
    }
    return redis_->get(make_continuity_world_key(logical_session_id));
}

std::optional<std::string> ChatService::load_continuity_world_owner(const std::string& world_id) {
    if (!redis_ || world_id.empty()) {
        return std::nullopt;
    }
    return redis_->get(make_continuity_world_owner_key(world_id));
}

std::optional<server::core::discovery::WorldLifecyclePolicy>
ChatService::load_continuity_world_policy(const std::string& world_id) {
    if (!redis_ || world_id.empty()) {
        return std::nullopt;
    }

    const auto payload = redis_->get(make_continuity_world_policy_key(world_id));
    if (!payload.has_value() || payload->empty()) {
        return std::nullopt;
    }
    return server::core::discovery::parse_world_lifecycle_policy(*payload);
}

std::optional<server::core::worlds::WorldMigrationEnvelope>
ChatService::load_continuity_world_migration(const std::string& world_id) {
    if (!redis_ || world_id.empty()) {
        return std::nullopt;
    }

    const auto payload = redis_->get(make_continuity_world_migration_key(world_id));
    if (!payload.has_value() || payload->empty()) {
        return std::nullopt;
    }
    return server::core::worlds::parse_world_migration_envelope(*payload);
}

std::optional<server::core::worlds::TopologyActuationRuntimeAssignmentItem>
ChatService::load_topology_runtime_assignment() const {
    if (!redis_ || continuity_.topology_runtime_assignment_key.empty() || continuity_.current_owner_id.empty()) {
        return std::nullopt;
    }

    try {
        const auto payload = redis_->get(continuity_.topology_runtime_assignment_key);
        if (!payload.has_value() || payload->empty()) {
            return std::nullopt;
        }

        const auto document = server::app::parse_topology_actuation_runtime_assignment_document(*payload);
        if (!document.has_value()) {
            return std::nullopt;
        }

        return server::app::find_topology_actuation_runtime_assignment_for_instance(
            *document,
            continuity_.current_owner_id);
    } catch (...) {
        return std::nullopt;
    }
}

std::string ChatService::current_runtime_default_world_id() const {
    const std::string fallback_world_id =
        continuity_.default_world_id.empty() ? std::string("default") : continuity_.default_world_id;
    const auto assignment = load_topology_runtime_assignment();
    const auto resolved =
        server::app::resolve_topology_runtime_assignment_world_id(fallback_world_id, assignment);
    return resolved.empty() ? std::string("default") : resolved;
}

ChatService::AppMigrationRoomHandoff
ChatService::resolve_app_world_migration_room_handoff(
    const server::core::worlds::WorldMigrationEnvelope& migration) const {
    AppMigrationRoomHandoff handoff{};
    if (migration.payload_kind != kAppMigrationPayloadRoomKind) {
        return handoff;
    }

    handoff.recognized = true;
    handoff.room = migration.payload_ref;
    return handoff;
}

void ChatService::persist_continuity_room(const std::string& logical_session_id,
                                          const std::string& room,
                                          std::uint64_t expires_unix_ms) {
    if (!redis_ || logical_session_id.empty() || room.empty() || expires_unix_ms == 0) {
        (void)continuity_state_write_fail_total_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (expires_unix_ms <= now_ms) {
        (void)redis_->del(make_continuity_room_key(logical_session_id));
        return;
    }

    const auto ttl_ms = expires_unix_ms - now_ms;
    const auto ttl_sec = static_cast<unsigned int>(std::max<std::uint64_t>(1, (ttl_ms + 999) / 1000));
    if (redis_->setex(make_continuity_room_key(logical_session_id), room, ttl_sec)) {
        (void)continuity_state_write_total_.fetch_add(1, std::memory_order_relaxed);
    } else {
        (void)continuity_state_write_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ChatService::persist_continuity_world(const std::string& logical_session_id,
                                           const std::string& world_id,
                                           std::uint64_t expires_unix_ms) {
    if (!redis_ || logical_session_id.empty() || world_id.empty() || expires_unix_ms == 0) {
        (void)continuity_world_write_fail_total_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (expires_unix_ms <= now_ms) {
        (void)redis_->del(make_continuity_world_key(logical_session_id));
        return;
    }

    const auto ttl_ms = expires_unix_ms - now_ms;
    const auto ttl_sec = static_cast<unsigned int>(std::max<std::uint64_t>(1, (ttl_ms + 999) / 1000));
    if (redis_->setex(make_continuity_world_key(logical_session_id), world_id, ttl_sec)) {
        (void)continuity_world_write_total_.fetch_add(1, std::memory_order_relaxed);
    } else {
        (void)continuity_world_write_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ChatService::persist_continuity_world_owner(const std::string& world_id,
                                                 const std::string& owner_id,
                                                 std::uint64_t expires_unix_ms) {
    if (!redis_ || world_id.empty() || owner_id.empty() || expires_unix_ms == 0) {
        (void)continuity_world_owner_write_fail_total_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (expires_unix_ms <= now_ms) {
        (void)redis_->del(make_continuity_world_owner_key(world_id));
        return;
    }

    const auto ttl_ms = expires_unix_ms - now_ms;
    const auto ttl_sec = static_cast<unsigned int>(std::max<std::uint64_t>(1, (ttl_ms + 999) / 1000));
    if (redis_->setex(make_continuity_world_owner_key(world_id), owner_id, ttl_sec)) {
        (void)continuity_world_owner_write_total_.fetch_add(1, std::memory_order_relaxed);
    } else {
        (void)continuity_world_owner_write_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::optional<ChatService::ContinuityLease> ChatService::try_resume_continuity_lease(std::string_view token) {
    if (!continuity_enabled()) {
        return std::nullopt;
    }

    const auto raw_token = extract_resume_token(token);
    if (!raw_token.has_value()) {
        return std::nullopt;
    }

    const std::string token_hash = sha256_hex(*raw_token);
    if (token_hash.empty()) {
        (void)continuity_lease_resume_fail_total_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    try {
        auto uow = db_pool_->make_repository_unit_of_work();
        auto session = uow->sessions().find_by_token_hash(token_hash);
        if (!session.has_value() || session->revoked_at_ms.has_value()) {
            (void)continuity_lease_resume_fail_total_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        if (session->expires_at_ms <= static_cast<std::int64_t>(now_ms)) {
            (void)continuity_lease_resume_fail_total_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        auto user = uow->users().find_by_id(session->user_id);
        if (!user.has_value() || user->name.empty()) {
            (void)continuity_lease_resume_fail_total_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        ContinuityLease lease;
        lease.logical_session_id = session->id;
        lease.resume_token = *raw_token;
        lease.user_id = session->user_id;
        lease.effective_user = user->name;
        const std::string fallback_world_id = current_runtime_default_world_id();
        bool world_restore_ok = false;
        bool preserve_room_on_restore = false;
        std::optional<std::string> migration_payload_room;
        const auto continuity_world = load_continuity_world(session->id);
        if (continuity_world.has_value() && !continuity_world->empty()) {
            const auto continuity_world_policy = load_continuity_world_policy(*continuity_world);
            const bool world_draining =
                continuity_world_policy.has_value() && continuity_world_policy->draining;
            const bool replacement_owner_matches_current =
                continuity_world_policy.has_value()
                && continuity_world_policy->replacement_owner_instance_id == continuity_.current_owner_id;
            const auto continuity_world_owner = load_continuity_world_owner(*continuity_world);
            const bool owner_present =
                continuity_world_owner.has_value() && !continuity_world_owner->empty();
            const bool owner_matches_current = continuity_world_owner.has_value()
                && !continuity_world_owner->empty()
                && *continuity_world_owner == continuity_.current_owner_id;
            if (owner_matches_current && (!world_draining || replacement_owner_matches_current)) {
                lease.world_id = *continuity_world;
                world_restore_ok = true;
                preserve_room_on_restore = true;
                (void)continuity_world_restore_total_.fetch_add(1, std::memory_order_relaxed);
                (void)continuity_world_owner_restore_total_.fetch_add(1, std::memory_order_relaxed);
            } else {
                bool migration_restored = false;
                if (const auto migration = load_continuity_world_migration(*continuity_world); migration.has_value()) {
                    const bool source_draining_for_migration = world_draining;
                    const bool target_owner_matches_current =
                        migration->target_owner_instance_id == continuity_.current_owner_id;
                    const bool current_backend_hosts_target_world =
                        current_runtime_default_world_id() == migration->target_world_id;
                    const auto target_world_owner = load_continuity_world_owner(migration->target_world_id);
                    const bool target_world_owner_matches_current =
                        target_world_owner.has_value()
                        && !target_world_owner->empty()
                        && *target_world_owner == continuity_.current_owner_id;

                    if (!source_draining_for_migration) {
                        (void)continuity_world_migration_restore_fallback_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        (void)continuity_world_migration_restore_fallback_source_not_draining_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    } else if (!target_owner_matches_current) {
                        (void)continuity_world_migration_restore_fallback_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        (void)continuity_world_migration_restore_fallback_target_owner_mismatch_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    } else if (!current_backend_hosts_target_world && !target_world_owner_matches_current) {
                        (void)continuity_world_migration_restore_fallback_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        (void)continuity_world_migration_restore_fallback_target_world_missing_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    } else {
                        lease.world_id = migration->target_world_id;
                        world_restore_ok = true;
                        preserve_room_on_restore = migration->preserve_room;
                        const auto room_handoff = resolve_app_world_migration_room_handoff(*migration);
                        if (room_handoff.recognized) {
                            if (!room_handoff.room.empty()) {
                                migration_payload_room = room_handoff.room;
                            } else {
                                (void)continuity_world_migration_payload_room_handoff_fallback_total_.fetch_add(
                                    1,
                                    std::memory_order_relaxed);
                            }
                        }
                        migration_restored = true;
                        (void)continuity_world_migration_restore_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                }

                if (!migration_restored) {
                    lease.world_id = fallback_world_id;
                    (void)continuity_world_restore_fallback_total_.fetch_add(1, std::memory_order_relaxed);
                    if (world_draining && !replacement_owner_matches_current) {
                        (void)continuity_world_restore_fallback_draining_replacement_unhonored_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    } else if (!owner_present) {
                        (void)continuity_world_restore_fallback_missing_owner_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        (void)continuity_world_owner_restore_fallback_total_.fetch_add(1, std::memory_order_relaxed);
                    } else if (!owner_matches_current) {
                        (void)continuity_world_restore_fallback_owner_mismatch_total_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        (void)continuity_world_owner_restore_fallback_total_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        } else {
            lease.world_id = fallback_world_id;
            (void)continuity_world_restore_fallback_total_.fetch_add(1, std::memory_order_relaxed);
            (void)continuity_world_restore_fallback_missing_world_total_.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        const auto continuity_room = load_continuity_room(session->id);
        if (world_restore_ok && migration_payload_room.has_value() && !migration_payload_room->empty()) {
            lease.room = *migration_payload_room;
            (void)continuity_world_migration_payload_room_handoff_total_.fetch_add(
                1,
                std::memory_order_relaxed);
        } else if (world_restore_ok
                   && preserve_room_on_restore
                   && continuity_room.has_value()
                   && !continuity_room->empty()) {
            lease.room = *continuity_room;
            (void)continuity_state_restore_total_.fetch_add(1, std::memory_order_relaxed);
        } else {
            lease.room = "lobby";
            (void)continuity_state_restore_fallback_total_.fetch_add(1, std::memory_order_relaxed);
        }
        lease.expires_unix_ms = static_cast<std::uint64_t>(session->expires_at_ms);
        lease.resumed = true;
        (void)continuity_lease_resume_total_.fetch_add(1, std::memory_order_relaxed);
        return lease;
    } catch (const std::exception& ex) {
        (void)continuity_lease_resume_fail_total_.fetch_add(1, std::memory_order_relaxed);
        corelog::warn(std::string("continuity resume lookup failed: ") + ex.what());
    } catch (...) {
        (void)continuity_lease_resume_fail_total_.fetch_add(1, std::memory_order_relaxed);
        corelog::warn("continuity resume lookup failed: unknown");
    }
    return std::nullopt;
}

std::optional<ChatService::ContinuityLease> ChatService::issue_continuity_lease(const std::string& user_id,
                                                                                 const std::string& effective_user,
                                                                                 const std::string& world_id,
                                                                                 const std::string& room,
                                                                                 const std::optional<std::string>& client_ip) {
    if (!continuity_enabled() || user_id.empty() || effective_user.empty()) {
        return std::nullopt;
    }

    try {
        const std::string raw_token = generate_uuid_v4();
        const std::string token_hash = sha256_hex(raw_token);
        if (token_hash.empty()) {
            (void)continuity_lease_issue_fail_total_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        const auto expires_at =
            std::chrono::system_clock::now() + std::chrono::seconds(continuity_.lease_ttl_sec);
        auto uow = db_pool_->make_repository_unit_of_work();
        auto session = uow->sessions().create(user_id, expires_at, client_ip, std::nullopt, token_hash);
        uow->commit();

        ContinuityLease lease;
        lease.logical_session_id = session.id;
        lease.resume_token = raw_token;
        lease.user_id = user_id;
        lease.effective_user = effective_user;
        lease.world_id = world_id.empty() ? current_runtime_default_world_id() : world_id;
        lease.room = room.empty() ? "lobby" : room;
        lease.expires_unix_ms = static_cast<std::uint64_t>(session.expires_at_ms);
        lease.resumed = false;
        persist_continuity_world(lease.logical_session_id, lease.world_id, lease.expires_unix_ms);
        persist_continuity_world_owner(lease.world_id, continuity_.current_owner_id, lease.expires_unix_ms);
        persist_continuity_room(lease.logical_session_id, lease.room, lease.expires_unix_ms);
        (void)continuity_lease_issue_total_.fetch_add(1, std::memory_order_relaxed);
        return lease;
    } catch (const std::exception& ex) {
        (void)continuity_lease_issue_fail_total_.fetch_add(1, std::memory_order_relaxed);
        corelog::warn(std::string("continuity lease issue failed: ") + ex.what());
    } catch (...) {
        (void)continuity_lease_issue_fail_total_.fetch_add(1, std::memory_order_relaxed);
        corelog::warn("continuity lease issue failed: unknown");
    }
    return std::nullopt;
}

// 사용자의 접속 상태(Presence)를 갱신합니다. (Redis SETEX)
void ChatService::touch_user_presence(const std::string& uid) {
    if (!redis_ || uid.empty()) {
        return;
    }
    redis_->setex(make_presence_key("presence:user:", uid), "1", presence_ttl());
}

// 임시 닉네임 생성 (UUID 기반 8자리)
std::string ChatService::gen_temp_name_uuid8() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint32_t v = static_cast<std::uint32_t>(rng());
    std::ostringstream oss; oss << std::hex; oss.width(8); oss.fill('0'); oss << v; return oss.str();
}

// 닉네임 중복 검사 및 임시 닉네임 할당
// desired가 비어있거나 "guest"인 경우 고유한 임시 닉네임을 생성하여 반환합니다.
// 이미 사용 중인 닉네임인 경우 에러를 전송하고 빈 문자열을 반환합니다.
std::string ChatService::ensure_unique_or_error(Session& s, const std::string& desired) {
    std::lock_guard<std::mutex> lk(state_.mu);
    if (!desired.empty() && desired != "guest") {
        auto itset = state_.by_user.find(desired);
        if (itset != state_.by_user.end()) {
            bool taken = false;
            for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                if (auto p = wit->lock()) { taken = true; break; }
                else { wit = itset->second.erase(wit); }
            }
            if (taken) {
                s.send_error(proto::errc::NAME_TAKEN, "name taken");
                return {};
            }
        }
        return desired;
    }
    // 임시 닉네임은 UUID의 앞 8자를 잘라 32비트 난수 근사로 사용한다.
    for (int i=0;i<4;++i) {
        std::string cand = gen_temp_name_uuid8();
        if (!state_.by_user.count(cand) || state_.by_user[cand].empty()) return cand;
    }
    return gen_temp_name_uuid8();
}

// 현재 활성화된 방 목록을 클라이언트에게 전송합니다.
// (system) 발신자로 채팅 메시지 형식을 빌려 목록을 전송합니다.
void ChatService::send_rooms_list(Session& s) {
    std::vector<std::uint8_t> body;
    std::string msg = "rooms:";

    // 1. Redis 데이터 미리 조회 (Lock 없이 수행)
    struct RoomInfo {
        std::string name;
        std::size_t count;
        bool is_locked;
    };
    std::vector<RoomInfo> redis_rooms;
    bool redis_available = false;

    if (redis_) {
        redis_available = true;
        std::vector<std::string> redis_rooms_list;
        redis_->smembers("rooms:active", redis_rooms_list);

        std::vector<std::string> password_keys;
        std::vector<std::string> user_count_keys;
        password_keys.reserve(redis_rooms_list.size());
        user_count_keys.reserve(redis_rooms_list.size());
        for (const auto& r : redis_rooms_list) {
            password_keys.push_back("room:password:" + r);
            user_count_keys.push_back("room:users:" + r);
        }

        std::vector<std::optional<std::string>> password_values;
        const bool password_batch_loaded = !password_keys.empty()
            && redis_->mget(password_keys, password_values)
            && password_values.size() == password_keys.size();

        std::vector<std::size_t> user_counts;
        const bool users_count_batch_loaded = !user_count_keys.empty()
            && redis_->scard_many(user_count_keys, user_counts)
            && user_counts.size() == user_count_keys.size();
        
        bool lobby_found = false;

        for (std::size_t i = 0; i < redis_rooms_list.size(); ++i) {
            const auto& r = redis_rooms_list[i];
            if (r == "lobby") lobby_found = true;

            std::size_t users_count = 0;
            if (users_count_batch_loaded) {
                users_count = user_counts[i];
            } else {
                const auto& users_key = user_count_keys[i];
                if (!redis_->scard(users_key, users_count)) {
                    std::vector<std::string> users;
                    redis_->smembers(users_key, users);
                    users_count = users.size();
                }
            }
            
            bool locked = false;
            if (password_batch_loaded) {
                locked = password_values[i].has_value();
            } else {
                auto pw = redis_->get("room:password:" + r);
                locked = pw.has_value();
            }
            
            redis_rooms.push_back({r, users_count, locked});
        }
        
        if (!lobby_found) {
            std::size_t users_count = 0;
            if (!redis_->scard("room:users:lobby", users_count)) {
                std::vector<std::string> users;
                redis_->smembers("room:users:lobby", users);
                users_count = users.size();
            }
            redis_rooms.push_back({"lobby", users_count, false});
        }
    }

    {
        // 2. 로컬 상태 처리 (Lock 필요)
        std::lock_guard<std::mutex> lk(state_.mu);

        // 로컬 방 정리
        std::vector<std::string> to_remove;
        for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
            std::size_t alive = 0;
            for (auto wit = it->second.begin(); wit != it->second.end(); ) { if (auto p = wit->lock()) { ++alive; ++wit; } else { wit = it->second.erase(wit); } }
            if (alive == 0 && it->first != std::string("lobby")) { to_remove.push_back(it->first); continue; }
        }
        for (auto& name : to_remove) { state_.rooms.erase(name); state_.room_passwords.erase(name); }

        if (redis_available) {
            // Redis 데이터 기반으로 메시지 구성
            for (auto& info : redis_rooms) {
                std::string display_name = info.name;
                bool is_locked = info.is_locked;

                // Redis에 잠금 정보가 없어도 로컬에 있을 수 있음
                if (!is_locked && state_.room_passwords.count(info.name)) {
                    is_locked = true;
                }

                if (is_locked) {
                    display_name = "🔒" + display_name;
                }
                msg += " " + display_name + "(" + std::to_string(info.count) + ")";
            }
        } else {
            // Fallback to local state
            for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
                std::size_t alive = it->second.size();
                std::string display_name = it->first;
                if (state_.room_passwords.count(it->first)) {
                    display_name = "🔒" + display_name;
                }
                msg += " " + display_name + "(" + std::to_string(alive) + ")";
            }
        }
    }
    // ChatBroadcast 메시지를 수동으로 직렬화합니다.
    // 편의 함수 대신 수동 직렬화를 사용하는 이유는,
    // (system) sender와 현재 타임스탬프(ts_ms)를 정확히 설정하기 위함입니다.
    server::wire::v1::ChatBroadcast pb; pb.set_room("(system)"); pb.set_sender("(system)"); pb.set_text(msg); pb.set_sender_sid(0);
    {
        auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        pb.set_ts_ms(static_cast<std::uint64_t>(now64));
    }
    {
        std::string bytes; pb.SerializeToString(&bytes);
        body.assign(bytes.begin(), bytes.end());
    }
    s.async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);
}

// 해당 방의 로컬 세션에게만 상태 갱신 알림을 전송합니다.
void ChatService::broadcast_refresh_local(const std::string& room) {
    std::vector<std::uint8_t> empty_body;
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.rooms.find(room);
        if (it != state_.rooms.end()) {
            collect_room_sessions(it->second, targets);
        }
    }

    for (auto& s : targets) {
        s->async_send(game_proto::MSG_REFRESH_NOTIFY, empty_body, 0);
    }
}

// 해당 방의 모든 유저에게 상태 갱신 알림을 전송합니다.
// 로컬 전송 + Redis Pub/Sub 전파
void ChatService::broadcast_refresh(const std::string& room) {
    // 1. 로컬 세션에게 전송
    broadcast_refresh_local(room);

    // 2. Redis Pub/Sub으로 다른 서버에 전파
    if (redis_ && pubsub_enabled()) {
        try {
            // fanout:refresh:<room> 채널 사용
            std::string channel = presence_.prefix + std::string("fanout:refresh:") + room;
            // Payload는 gwid만 있으면 됨 (self-echo 방지용)
            std::string message = "gw=" + gateway_id_;
            redis_->publish(channel, std::move(message));
        } catch (...) {}
    }
}

// 특정 방의 사용자 목록을 클라이언트에게 전송합니다.
// 잠긴 방의 경우 멤버가 아니면 목록을 볼 수 없습니다.
void ChatService::send_room_users(Session& s, const std::string& target) {
    std::vector<std::string> names;
    bool allow = true;
    std::string viewer;

    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(target);
        bool is_locked = state_.room_passwords.count(target) > 0;
        bool is_member = false;

        // 로컬 세션에서 멤버 여부 확인
        if (itroom != state_.rooms.end()) {
            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                if (auto p = wit->lock()) {
                    if (p.get() == &s) {
                        is_member = true;
                        break;
                    }
                    ++wit;
                } else {
                    wit = itroom->second.erase(wit);
                }
            }
        }

        if (is_locked && !is_member) {
            allow = false;
        }

        if (auto viewer_it = state_.user.find(&s); viewer_it != state_.user.end()) {
            viewer = viewer_it->second;
        }
    }

    if (!allow) {
        send_system_notice(s, "room is locked");
        return;
    }

    // Redis에서 전체 사용자 목록 조회 (분산 환경 지원)
    if (redis_) {
        std::vector<std::string> redis_users;
        if (redis_->smembers("room:users:" + target, redis_users)) {
            names = std::move(redis_users);
        }
    }

    // Redis가 없거나 실패한 경우 로컬 상태를 fallback으로 사용
    if (names.empty()) {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(target);
        if (itroom != state_.rooms.end()) {
            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                if (auto p = wit->lock()) {
                    auto itu = state_.user.find(p.get());
                    std::string name = (itu != state_.user.end()) ? itu->second : std::string("guest");
                    names.push_back(std::move(name));
                    ++wit;
                } else {
                    wit = itroom->second.erase(wit);
                }
            }
        }
    }

    server::wire::v1::RoomUsers pb;
    pb.set_room(target);
    std::unordered_set<std::string> blocked;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (auto it = state_.user_blacklists.find(viewer); it != state_.user_blacklists.end()) {
            blocked = it->second;
        }
    }
    for (const auto& name : names) {
        if (blocked.count(name) > 0) {
            continue;
        }
        pb.add_users(name);
    }
    std::string bytes;
    pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
    s.async_send(game_proto::MSG_ROOM_USERS, body, 0);
}

// refresh/snapshot은 클라이언트가 UI를 다시 기준 상태(authoritative state)로 맞추는 복구 경로다.
// 방 목록, 참여자 목록, 최근 메시지, 읽음 위치를 분리해서 보내면 중간 실패 시 UI가 서로 다른 시점의 상태를 섞어 보게 된다.
void ChatService::send_snapshot(Session& s, const std::string& current) {
    std::vector<std::uint8_t> body;
    server::wire::v1::StateSnapshot pb; pb.set_current_room(current);

    // 분산 캐시는 먼저 조회하되, 락을 잡은 채 네트워크 저장소를 기다리지는 않는다.
    // 그렇지 않으면 한 사용자의 refresh가 전체 방 상태 락을 오래 붙잡아 다른 요청까지 지연시킬 수 있다.
    struct RoomInfo {
        std::string name;
        std::size_t count;
        bool is_locked;
    };
    std::vector<RoomInfo> redis_rooms;
    bool redis_available = false;

    if (redis_) {
        redis_available = true;
        std::vector<std::string> active_rooms;
        redis_->smembers("rooms:active", active_rooms);

        std::vector<std::string> password_keys;
        std::vector<std::string> user_count_keys;
        password_keys.reserve(active_rooms.size());
        user_count_keys.reserve(active_rooms.size());
        for (const auto& r : active_rooms) {
            password_keys.push_back("room:password:" + r);
            user_count_keys.push_back("room:users:" + r);
        }

        std::vector<std::optional<std::string>> password_values;
        const bool password_batch_loaded = !password_keys.empty()
            && redis_->mget(password_keys, password_values)
            && password_values.size() == password_keys.size();

        std::vector<std::size_t> user_counts;
        const bool users_count_batch_loaded = !user_count_keys.empty()
            && redis_->scard_many(user_count_keys, user_counts)
            && user_counts.size() == user_count_keys.size();

        bool lobby_found = false;
        for (std::size_t i = 0; i < active_rooms.size(); ++i) {
            const auto& r = active_rooms[i];
            if (r == "lobby") lobby_found = true;

            std::size_t users_count = 0;
            if (users_count_batch_loaded) {
                users_count = user_counts[i];
            } else {
                const auto& users_key = user_count_keys[i];
                if (!redis_->scard(users_key, users_count)) {
                    std::vector<std::string> users;
                    redis_->smembers(users_key, users);
                    users_count = users.size();
                }
            }
            
            bool locked = false;
            if (password_batch_loaded) {
                locked = password_values[i].has_value();
            } else {
                auto pw = redis_->get("room:password:" + r);
                locked = pw.has_value();
            }
            
            redis_rooms.push_back({r, users_count, locked});
        }
        
        if (!lobby_found) {
            std::size_t users_count = 0;
            if (!redis_->scard("room:users:lobby", users_count)) {
                std::vector<std::string> users;
                redis_->smembers("room:users:lobby", users);
                users_count = users.size();
            }
            redis_rooms.push_back({"lobby", users_count, false});
        }
    }

    {
        // 2. 로컬 상태 처리 (Lock 필요)
        std::lock_guard<std::mutex> lk(state_.mu);

        // 로컬 방 정리
        std::vector<std::string> to_remove;
        for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
            std::uint32_t alive = 0; for (auto wit = it->second.begin(); wit != it->second.end(); ) { if (auto p = wit->lock()) { ++alive; ++wit; } else { wit = it->second.erase(wit); } }
            if (alive == 0 && it->first != std::string("lobby")) { to_remove.push_back(it->first); continue; }
        }
        for (auto& name : to_remove) { state_.rooms.erase(name); state_.room_passwords.erase(name); }

        if (redis_available) {
            // Redis 데이터 기반으로 메시지 구성
            for (auto& info : redis_rooms) {
                auto* ri = pb.add_rooms();
                ri->set_name(info.name);
                ri->set_members(info.count);

                bool locked = info.is_locked;
                if (!locked && state_.room_passwords.count(info.name)) {
                    locked = true;
                }
                ri->set_locked(locked);
            }
        } else {
            // Fallback to local state
            for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
                std::size_t alive = it->second.size();
                auto* ri = pb.add_rooms(); ri->set_name(it->first); ri->set_members(alive); ri->set_locked(state_.room_passwords.count(it->first) > 0);
            }
        }
    }

    std::string viewer_name;
    std::unordered_set<std::string> viewer_blocked;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (auto it = state_.user.find(&s); it != state_.user.end()) {
            viewer_name = it->second;
            if (auto blk_it = state_.user_blacklists.find(viewer_name); blk_it != state_.user_blacklists.end()) {
                viewer_blocked = blk_it->second;
            }
        }
    }

    // 3. 현재 방 유저 목록 조회 (Redis 우선, Fallback 로컬)
    {
        std::vector<std::string> user_list;
        bool loaded_from_redis = false;

        if (redis_) {
            if (redis_->smembers("room:users:" + current, user_list)) {
                loaded_from_redis = true;
            }
        }

        if (loaded_from_redis) {
            for (const auto& name : user_list) {
                if (viewer_blocked.count(name) > 0) {
                    continue;
                }
                pb.add_users(name);
            }
        } else {
            // Fallback to local state
            std::lock_guard<std::mutex> lk(state_.mu);
            auto itroom = state_.rooms.find(current);
            if (itroom != state_.rooms.end()) {
                for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                    if (auto p = wit->lock()) {
                        auto itu = state_.user.find(p.get());
                        std::string name = (itu != state_.user.end()) ? itu->second : std::string("guest");
                        if (viewer_blocked.count(name) > 0) {
                            ++wit;
                            continue;
                        }
                        pb.add_users(name); ++wit;
                    } else { wit = itroom->second.erase(wit); }
                }
            }
        }
    }

    // 최근 메시지는 Redis를 우선 쓰고 부족한 만큼만 DB로 메운다.
    // 이렇게 해야 평소에는 빠르고, 캐시가 비었거나 일부만 있을 때도 기능은 유지된다.
    std::string rid;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.room_ids.find(current);
        if (it != state_.room_ids.end()) {
            rid = it->second;
        }
    }
    if (rid.empty() && db_pool_) {
        rid = ensure_room_id_ci(current);
    }

    std::unordered_set<std::uint64_t> added_ids;
    bool loaded_from_cache = false;
    std::size_t cached_messages = 0;
    if (redis_ && !rid.empty()) {
        std::vector<server::wire::v1::StateSnapshot::SnapshotMessage> cached;
        if (load_recent_messages_from_cache(rid, cached)) {
            for (auto& message : cached) {
                if (added_ids.count(message.id())) continue; // 중복 제거
                auto* sm = pb.add_messages();
                *sm = message;
                added_ids.insert(message.id());
            }
            cached_messages = cached.size();
            loaded_from_cache = (cached_messages >= history_.recent_limit);
        }
    }

    std::uint64_t last_seen_value = 0;
    bool last_seen_loaded = false;
    if (db_pool_ && !rid.empty()) {
        try {
            std::string uid;
            {
                std::lock_guard<std::mutex> lk(state_.mu);
                auto itu = state_.user_uuid.find(&s);
                if (itu != state_.user_uuid.end()) uid = itu->second;
            }

            auto uow = db_pool_->make_repository_unit_of_work();
            if (!uid.empty()) {
                auto opt = uow->memberships().get_last_seen(uid, rid);
                last_seen_value = opt.value_or(0);
            }
            last_seen_loaded = true;
            pb.set_last_seen_id(last_seen_value);

            // 캐시된 메시지가 부족하면 DB에서 추가로 조회합니다.
            if (!loaded_from_cache || cached_messages < history_.recent_limit) {
                const std::size_t limit = history_.recent_limit;
                const std::size_t fetch_factor = history_.fetch_factor;
                const std::size_t fetch_span = limit * fetch_factor;
                const std::size_t fetch_count = std::min(history_.max_list_len, std::max(limit, fetch_span));

                auto last_id = uow->messages().get_last_id(rid);
                std::uint64_t since_id = 0;
                // 마지막으로 읽은 메시지(last_seen)를 기준으로 가져올 범위를 계산합니다.
                    if (last_id > 0) {
                        // last_seen_value(마지막으로 읽은 메시지)를 기준으로 Fetch 범위를 결정합니다.
                        // 1. last_seen이 없으면(0) 최신 N개를 가져옵니다.
                        // 2. last_seen이 너무 오래되어 Gap이 크면, 중간을 건너뛰고 최신 메시지 위주로 가져옵니다.
                        // 3. 정상적인 경우 last_seen 직후부터 가져옵니다.
                        if (last_seen_value == 0) {
                            since_id = (last_id > limit) ? (last_id - limit) : 0;
                        } else if (last_seen_value >= last_id) {
                            since_id = (last_id > limit) ? (last_id - limit) : 0;
                        } else {
                            std::uint64_t context = static_cast<std::uint64_t>(limit) * static_cast<std::uint64_t>(fetch_factor);
                            if (last_id > context) {
                                std::uint64_t cut = last_id - context;
                                since_id = (last_seen_value > cut) ? last_seen_value : cut;
                            } else {
                                since_id = last_seen_value;
                            }
                        }
                    }

                auto msgs = uow->messages().fetch_recent_by_room(rid, since_id, fetch_count);
                // DB에서 가져온 것 중 이미 캐시에 있는 것은 제외
                std::vector<server::storage::Message> filtered;
                filtered.reserve(msgs.size());
                for (const auto& m : msgs) {
                    if (added_ids.find(m.id) == added_ids.end()) {
                        server::storage::Message m_copy = m;
                        filtered.push_back(std::move(m_copy));
                    }
                }
                msgs = std::move(filtered);

                if (msgs.size() > limit) {
                    msgs.erase(msgs.begin(), msgs.end() - static_cast<std::ptrdiff_t>(limit));
                }
                std::size_t budget = (cached_messages >= limit) ? 0 : (limit - cached_messages);
                if (budget == 0) {
                    msgs.clear();
                } else if (msgs.size() > budget) {
                    msgs.erase(msgs.begin(), msgs.end() - static_cast<std::ptrdiff_t>(budget));
                }
                for (const auto& m : msgs) {
                    auto* sm = pb.add_messages();
                    sm->set_id(m.id);
                    std::string sender;
                    if (m.user_name && !m.user_name->empty()) sender = *m.user_name;
                    else sender = std::string("(system)");
                    sm->set_sender(sender);
                    sm->set_text(m.content);
                    sm->set_ts_ms(static_cast<std::uint64_t>(m.created_at_ms));
                    // DB에서 가져온 메시지를 Redis 캐시에 채워넣습니다 (Read-Repair).
                    if (redis_) {
                        cache_recent_message(rid, *sm);
                    }
                    added_ids.insert(m.id);
                }
            }
        } catch (const std::exception& e) {
            corelog::warn(std::string("recent history DB fallback failed: ") + e.what());
        } catch (...) {
            corelog::warn("recent history DB fallback failed: unknown error");
        }
    }
    if (!last_seen_loaded) {
        pb.set_last_seen_id(last_seen_value);
    }

    // 현재 세션의 이름(닉네임)을 클라이언트에게 알려줌 (Guest 식별용)
    std::string my_name;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.user.find(&s);
        if (it != state_.user.end()) my_name = it->second;
    }
    if (!my_name.empty()) {
        pb.set_your_name(my_name);
    }

    {
        std::string bytes; pb.SerializeToString(&bytes);
        body.assign(bytes.begin(), bytes.end());
    }
    s.async_send(game_proto::MSG_STATE_SNAPSHOT, body, 0);
}

// 외부에서 수신한 브로드캐스트(예: Redis Pub/Sub)를 해당 방의 로컬 세션들에게 전달합니다.
void ChatService::broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, Session* self) {
    (void)self;
    std::string sender;
    {
        server::wire::v1::ChatBroadcast pb;
        if (server::wire::codec::Decode(body.data(), body.size(), pb)) {
            sender = pb.sender();
        }
    }

    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.rooms.find(room);
        if (it != state_.rooms.end()) {
            collect_room_sessions(it->second, targets);
        }

        if (!sender.empty()) {
            std::vector<std::shared_ptr<Session>> filtered_targets;
            filtered_targets.reserve(targets.size());
            for (auto& target : targets) {
                auto receiver_it = state_.user.find(target.get());
                if (receiver_it == state_.user.end()) {
                    continue;
                }
                const std::string& receiver = receiver_it->second;
                if (auto blk_it = state_.user_blacklists.find(receiver);
                    blk_it != state_.user_blacklists.end() && blk_it->second.count(sender) > 0) {
                    continue;
                }
                filtered_targets.push_back(target);
            }
            targets = std::move(filtered_targets);
        }
    }
    for (auto& t : targets) {
        int f = 0; // 재전파에서는 self 플래그를 사용하지 않는다.
        t->async_send(game_proto::MSG_CHAT_BROADCAST, body, f);
    }
}

// 시스템 공지 메시지를 전송합니다.
void ChatService::send_system_notice(Session& s, const std::string& text) {
    server::wire::v1::ChatBroadcast pb;
    pb.set_room("(system)");
    pb.set_sender("(system)");
    pb.set_text(text);
    pb.set_sender_sid(0);
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    pb.set_ts_ms(static_cast<std::uint64_t>(now64));
    std::string bytes;
    pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
    s.async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);
}

ChatService::LuaColdHookOutcome ChatService::invoke_lua_cold_hook(
    std::string_view hook_name,
    const server::core::scripting::LuaHookContext& context) {
    LuaColdHookOutcome outcome{};
    if (!lua_runtime_) {
        return outcome;
    }

    const auto runtime_metrics = lua_runtime_->metrics_snapshot();
    {
        std::lock_guard<std::mutex> lock(lua_hook_metrics_mu_);
        if (runtime_metrics.reload_epoch != lua_reload_epoch_) {
            lua_reload_epoch_ = runtime_metrics.reload_epoch;
            lua_hook_consecutive_failures_.clear();
            lua_hook_disabled_.clear();
            corelog::info("lua cold hook state reset after script reload epoch=" + std::to_string(lua_reload_epoch_));
        }

        if (lua_hook_disabled_.count(std::string(hook_name)) > 0) {
            return outcome;
        }
    }

    const auto call_started_at = std::chrono::steady_clock::now();
    server::core::scripting::LuaRuntime::CallAllResult call_result{};
    if (lua_execution_strand_ && !lua_execution_strand_->running_in_this_thread()) {
        auto promise = std::make_shared<std::promise<server::core::scripting::LuaRuntime::CallAllResult>>();
        auto future = promise->get_future();
        asio::post(*lua_execution_strand_, [this,
                                            hook = std::string(hook_name),
                                            context,
                                            promise]() mutable {
            promise->set_value(lua_runtime_->call_all(hook, context));
        });
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            if (io_ == nullptr || io_->poll_one() == 0) {
                std::this_thread::yield();
            }
        }
        call_result = future.get();
    } else {
        call_result = lua_runtime_->call_all(std::string(hook_name), context);
    }
    const auto call_elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - call_started_at)
            .count());

    const bool call_failed = (!call_result.error.empty() || call_result.failed != 0);
    {
        std::lock_guard<std::mutex> lock(lua_hook_metrics_mu_);
        const std::string hook_key(hook_name);
        lua_hook_calls_total_[hook_key] += call_result.attempted;
        lua_hook_errors_total_[hook_key] += call_result.failed;

        if (call_result.failed == 0 && !call_result.error.empty()) {
            ++lua_hook_errors_total_[hook_key];
        }

        auto& script_calls = lua_hook_script_calls_total_[hook_key];
        auto& script_errors = lua_hook_script_errors_total_[hook_key];
        for (const auto& script_result : call_result.script_results) {
            const std::string script_name = script_result.env_name.empty()
                ? std::string("(unknown)")
                : script_result.env_name;
            ++script_calls[script_name];
            if (script_result.failed) {
                ++script_errors[script_name];
            }

            switch (script_result.failure_kind) {
            case server::core::scripting::LuaRuntime::ScriptFailureKind::kInstructionLimit:
                ++lua_hook_instruction_limit_hits_[hook_key];
                break;
            case server::core::scripting::LuaRuntime::ScriptFailureKind::kMemoryLimit:
                ++lua_hook_memory_limit_hits_[hook_key];
                break;
            case server::core::scripting::LuaRuntime::ScriptFailureKind::kNone:
            case server::core::scripting::LuaRuntime::ScriptFailureKind::kOther:
            default:
                break;
            }
        }

        auto& consecutive = lua_hook_consecutive_failures_[hook_key];
        if (call_failed) {
            ++consecutive;
            if (lua_auto_disable_threshold_ > 0
                && consecutive >= lua_auto_disable_threshold_
                && lua_hook_disabled_.count(hook_key) == 0) {
                lua_hook_disabled_.insert(hook_key);
                ++lua_hook_auto_disable_total_[hook_key];
                corelog::warn(
                    "lua cold hook auto-disabled hook=" + hook_key
                    + " consecutive_failures=" + std::to_string(consecutive)
                    + " threshold=" + std::to_string(lua_auto_disable_threshold_));
            }
        } else {
            consecutive = 0;
        }
    }

    if (!call_result.error.empty()) {
        corelog::warn(
            "lua cold hook call failed hook=" + std::string(hook_name)
            + " reason=" + call_result.error);
    }

    if (call_result.failed != 0) {
        corelog::warn(
            "lua cold hook call had failures hook=" + std::string(hook_name)
            + " failed=" + std::to_string(call_result.failed));
    }

    if (lua_hook_warn_budget_us_ > 0 && call_elapsed_ns > lua_hook_warn_budget_us_ * 1'000ULL) {
        corelog::warn(
            "lua cold hook latency budget exceeded hook=" + std::string(hook_name)
            + " elapsed_us=" + std::to_string(call_elapsed_ns / 1'000ULL)
            + " budget_us=" + std::to_string(lua_hook_warn_budget_us_));
    }

    outcome.notices = call_result.notices;

    switch (call_result.decision) {
    case server::core::scripting::LuaHookDecision::kPass:
    case server::core::scripting::LuaHookDecision::kAllow:
    case server::core::scripting::LuaHookDecision::kModify:
    case server::core::scripting::LuaHookDecision::kHandled:
        break;
    case server::core::scripting::LuaHookDecision::kBlock:
    case server::core::scripting::LuaHookDecision::kDeny:
        outcome.stop_default = true;
        outcome.deny_reason = call_result.reason;
        break;
    default:
        break;
    }

    return outcome;
}

bool ChatService::maybe_handle_chat_hook_plugin(Session& s,
                                                const std::string& room,
                                                const std::string& sender,
                                                std::string& text) {
    if (!hook_plugin_) {
        return false;
    }

    auto out = hook_plugin_->chain.on_chat_send(s.session_id(), room, sender, text);
    for (const auto& notice : out.notices) {
        if (!notice.empty()) {
            send_system_notice(s, notice);
        }
    }
    return out.stop_default;
}

bool ChatService::maybe_handle_login_hook(Session& s, const std::string& user) {
    server::core::scripting::LuaHookContext ctx{};
    ctx.session_id = s.session_id();
    ctx.user = user;

    if (!hook_plugin_) {
        const auto lua_out = invoke_lua_cold_hook("on_login", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                send_system_notice(s, notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        const std::string reason = lua_out.deny_reason.empty() ? "login denied by lua script" : lua_out.deny_reason;
        s.send_error(proto::errc::FORBIDDEN, reason);
        return true;
    }

    const auto out = hook_plugin_->chain.on_login(s.session_id(), user);
    for (const auto& notice : out.notices) {
        if (!notice.empty()) {
            send_system_notice(s, notice);
        }
    }

    if (!out.stop_default) {
        const auto lua_out = invoke_lua_cold_hook("on_login", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                send_system_notice(s, notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        const std::string reason = lua_out.deny_reason.empty() ? "login denied by lua script" : lua_out.deny_reason;
        s.send_error(proto::errc::FORBIDDEN, reason);
        return true;
    }

    const std::string reason = out.deny_reason.empty() ? "login denied by plugin" : out.deny_reason;
    s.send_error(proto::errc::FORBIDDEN, reason);
    return true;
}

bool ChatService::maybe_handle_join_hook(Session& s, const std::string& user, const std::string& room) {
    server::core::scripting::LuaHookContext ctx{};
    ctx.session_id = s.session_id();
    ctx.user = user;
    ctx.room = room;

    if (!hook_plugin_) {
        const auto lua_out = invoke_lua_cold_hook("on_join", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                send_system_notice(s, notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        const std::string reason = lua_out.deny_reason.empty() ? "join denied by lua script" : lua_out.deny_reason;
        s.send_error(proto::errc::FORBIDDEN, reason);
        return true;
    }

    const auto out = hook_plugin_->chain.on_join(s.session_id(), user, room);
    for (const auto& notice : out.notices) {
        if (!notice.empty()) {
            send_system_notice(s, notice);
        }
    }

    if (!out.stop_default) {
        const auto lua_out = invoke_lua_cold_hook("on_join", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                send_system_notice(s, notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        const std::string reason = lua_out.deny_reason.empty() ? "join denied by lua script" : lua_out.deny_reason;
        s.send_error(proto::errc::FORBIDDEN, reason);
        return true;
    }

    const std::string reason = out.deny_reason.empty() ? "join denied by plugin" : out.deny_reason;
    s.send_error(proto::errc::FORBIDDEN, reason);
    return true;
}

bool ChatService::maybe_handle_leave_hook(Session& s, const std::string& user, const std::string& room) {
    server::core::scripting::LuaHookContext ctx{};
    ctx.session_id = s.session_id();
    ctx.user = user;
    ctx.room = room;

    if (!hook_plugin_) {
        const auto lua_out = invoke_lua_cold_hook("on_leave", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                send_system_notice(s, notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        const std::string reason = lua_out.deny_reason.empty() ? "leave denied by lua script" : lua_out.deny_reason;
        s.send_error(proto::errc::FORBIDDEN, reason);
        return true;
    }

    const auto out = hook_plugin_->chain.on_leave(s.session_id(), user, room);
    for (const auto& notice : out.notices) {
        if (!notice.empty()) {
            send_system_notice(s, notice);
        }
    }

    if (!out.stop_default) {
        const auto lua_out = invoke_lua_cold_hook("on_leave", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                send_system_notice(s, notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        const std::string reason = lua_out.deny_reason.empty() ? "leave denied by lua script" : lua_out.deny_reason;
        s.send_error(proto::errc::FORBIDDEN, reason);
        return true;
    }

    const std::string reason = out.deny_reason.empty() ? "leave denied by plugin" : out.deny_reason;
    s.send_error(proto::errc::FORBIDDEN, reason);
    return true;
}

void ChatService::notify_session_event_hook(std::uint32_t session_id,
                                            SessionEventKindV2 kind,
                                            const std::string& user,
                                            const std::string& reason) {
    server::core::scripting::LuaHookContext ctx{};
    ctx.session_id = session_id;
    ctx.user = user;
    ctx.reason = reason;
    ctx.event = lua_session_event_name(kind);

    if (!hook_plugin_) {
        const auto lua_out = invoke_lua_cold_hook("on_session_event", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                corelog::info("lua session_event notice: " + notice);
            }
        }
        if (lua_out.stop_default) {
            corelog::warn("lua on_session_event requested stop_default; ignored for cleanup safety");
        }
        return;
    }

    const auto out = hook_plugin_->chain.on_session_event(session_id, kind, user, reason);
    for (const auto& notice : out.notices) {
        if (!notice.empty()) {
            corelog::info("chat_hook session_event notice: " + notice);
        }
    }
    if (out.stop_default) {
        corelog::warn("chat_hook session_event requested stop_default; skipping lua cold hook because native did not pass");
        return;
    }

    const auto lua_out = invoke_lua_cold_hook("on_session_event", ctx);
    for (const auto& notice : lua_out.notices) {
        if (!notice.empty()) {
            corelog::info("lua session_event notice: " + notice);
        }
    }
    if (lua_out.stop_default) {
        corelog::warn("lua on_session_event requested stop_default; ignored for cleanup safety");
    }
}

bool ChatService::maybe_handle_admin_command_hook(std::string_view command,
                                                  std::string_view issuer,
                                                  std::string_view payload_json,
                                                  std::string_view args,
                                                  std::string& deny_reason) {
    deny_reason.clear();
    server::core::scripting::LuaHookContext ctx{};
    ctx.command = std::string(command);
    ctx.issuer = std::string(issuer);
    ctx.payload_json = std::string(payload_json);
    ctx.args = std::string(args);

    if (!hook_plugin_) {
        const auto lua_out = invoke_lua_cold_hook("on_admin_command", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                corelog::info("lua admin notice: " + notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        deny_reason = lua_out.deny_reason.empty() ? "admin command denied by lua script" : lua_out.deny_reason;
        return true;
    }

    const auto out = hook_plugin_->chain.on_admin_command(command, issuer, payload_json);
    for (const auto& notice : out.notices) {
        if (!notice.empty()) {
            corelog::info("chat_hook admin notice: " + notice);
        }
    }
    if (!out.response_json.empty()) {
        corelog::info("chat_hook admin response_json: " + out.response_json);
    }

    if (!out.stop_default) {
        const auto lua_out = invoke_lua_cold_hook("on_admin_command", ctx);
        for (const auto& notice : lua_out.notices) {
            if (!notice.empty()) {
                corelog::info("lua admin notice: " + notice);
            }
        }
        if (!lua_out.stop_default) {
            return false;
        }

        deny_reason = lua_out.deny_reason.empty() ? "admin command denied by lua script" : lua_out.deny_reason;
        return true;
    }

    deny_reason = out.deny_reason.empty() ? "admin command denied by plugin" : out.deny_reason;
    return true;
}

void ChatService::admin_disconnect_users(const std::vector<std::string>& users, const std::string& reason) {
    if (users.empty()) {
        return;
    }

    std::vector<std::string> targets;
    targets.reserve(users.size());
    for (const auto& user : users) {
        if (!user.empty()) {
            targets.push_back(user);
        }
    }
    if (targets.empty()) {
        return;
    }

    {
        std::string deny_reason;
        const std::string payload_json =
            std::string("{\"users\":") + json_array_of_strings(targets) +
            ",\"reason\":\"" + json_escape(reason) + "\"}";
        if (maybe_handle_admin_command_hook("disconnect_users", "control-plane", payload_json, {}, deny_reason)) {
            corelog::warn("admin disconnect denied by plugin: " + deny_reason);
            return;
        }
    }

    const std::string notice = reason;
    if (!job_queue_.TryPush([this, targets = std::move(targets), notice]() {
        std::vector<std::shared_ptr<Session>> sessions;
        std::unordered_set<Session*> seen;

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            for (const auto& user : targets) {
                auto itset = state_.by_user.find(user);
                if (itset == state_.by_user.end()) {
                    continue;
                }

                for (auto wit = itset->second.begin(); wit != itset->second.end();) {
                    if (auto session = wit->lock()) {
                        if (!state_.authed.count(session.get()) || state_.guest.count(session.get())) {
                            ++wit;
                            continue;
                        }
                        if (seen.insert(session.get()).second) {
                            sessions.push_back(std::move(session));
                        }
                        ++wit;
                    } else {
                        wit = itset->second.erase(wit);
                    }
                }
            }
        }

        for (auto& session : sessions) {
            if (!notice.empty()) {
                send_system_notice(*session, notice);
            }
            session->stop();
        }

        corelog::info("admin disconnect applied: requested=" + std::to_string(targets.size()) +
                      " disconnected=" + std::to_string(sessions.size()));
    })) {
        corelog::warn("admin disconnect dropped: job queue full");
    }
}

void ChatService::admin_broadcast_notice(const std::string& text) {
    if (text.empty()) {
        return;
    }

    {
        std::string deny_reason;
        const std::string payload_json =
            std::string("{\"text\":\"") + json_escape(text) + "\"}";
        if (maybe_handle_admin_command_hook("announce", "control-plane", payload_json, text, deny_reason)) {
            corelog::warn("admin broadcast denied by plugin: " + deny_reason);
            return;
        }
    }

    const std::string notice = text;
    if (!job_queue_.TryPush([this, notice]() {
        std::vector<std::shared_ptr<Session>> sessions;
        std::unordered_set<Session*> seen;

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            for (auto& [_, room_set] : state_.rooms) {
                for (auto wit = room_set.begin(); wit != room_set.end();) {
                    if (auto session = wit->lock()) {
                        if (!state_.authed.count(session.get()) || state_.guest.count(session.get())) {
                            ++wit;
                            continue;
                        }
                        if (seen.insert(session.get()).second) {
                            sessions.push_back(std::move(session));
                        }
                        ++wit;
                    } else {
                        wit = room_set.erase(wit);
                    }
                }
            }
        }

        for (auto& session : sessions) {
            send_system_notice(*session, notice);
        }

        corelog::info("admin announcement delivered: sessions=" + std::to_string(sessions.size()));
    })) {
        corelog::warn("admin announcement dropped: job queue full");
    }
}

void ChatService::admin_apply_runtime_setting(const std::string& key, const std::string& value) {
    if (key.empty() || value.empty()) {
        return;
    }

    {
        std::string deny_reason;
        const std::string payload_json =
            std::string("{\"key\":\"") + json_escape(key) +
            "\",\"value\":\"" + json_escape(value) + "\"}";
        const std::string args = key + "=" + value;
        if (maybe_handle_admin_command_hook("apply_runtime_setting", "control-plane", payload_json, args, deny_reason)) {
            corelog::warn("admin runtime setting denied by plugin: " + deny_reason);
            return;
        }
    }

    const auto request_started_at = std::chrono::steady_clock::now();
    server::core::runtime_metrics::record_runtime_setting_reload_attempt();

    if (!job_queue_.TryPush([this, key, value, request_started_at]() {
        auto trim_ascii_local = [](std::string_view input) {
            std::size_t begin = 0;
            while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
                ++begin;
            }
            std::size_t end = input.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
                --end;
            }
            return std::string(input.substr(begin, end - begin));
        };

        auto to_lower_ascii_local = [](std::string_view input) {
            std::string out(input);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        };

        auto parse_u32 = [](std::string_view raw) -> std::optional<std::uint32_t> {
            if (raw.empty()) {
                return std::nullopt;
            }
            try {
                std::size_t pos = 0;
                const auto parsed = std::stoull(std::string(raw), &pos, 10);
                if (pos != raw.size() || parsed > std::numeric_limits<std::uint32_t>::max()) {
                    return std::nullopt;
                }
                return static_cast<std::uint32_t>(parsed);
            } catch (...) {
                server::core::runtime_metrics::record_exception_ignored();
                return std::nullopt;
            }
        };

        const auto finalize_failure = [&](const std::string& reason, std::string_view key_name) {
            server::core::runtime_metrics::record_runtime_setting_reload_failure();
            server::core::runtime_metrics::record_runtime_setting_reload_latency(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - request_started_at));
            corelog::warn("admin setting rejected: key=" + std::string(key_name) + " reason=" + reason);
        };

        const auto finalize_success = [&](std::string_view key_name, std::uint32_t applied_value) {
            server::core::runtime_metrics::record_runtime_setting_reload_success();
            server::core::runtime_metrics::record_runtime_setting_reload_latency(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - request_started_at));
            corelog::info(
                "admin setting applied: key=" + std::string(key_name) + " value=" + std::to_string(applied_value));
        };

        const std::string normalized_key = to_lower_ascii_local(trim_ascii_local(key));
        const std::string normalized_value = trim_ascii_local(value);
        const auto parsed = parse_u32(normalized_value);
        if (!parsed) {
            finalize_failure("invalid_value", normalized_key);
            return;
        }

        const auto* setting_rule = server::core::config::find_runtime_setting_rule(normalized_key);
        if (setting_rule == nullptr) {
            finalize_failure("unsupported_key", normalized_key);
            return;
        }

        std::uint32_t min_allowed = setting_rule->min_value;
        if (setting_rule->key_id == server::core::config::RuntimeSettingKey::kRoomRecentMaxlen) {
            min_allowed = std::max(min_allowed, static_cast<std::uint32_t>(history_.recent_limit));
        }

        if (*parsed < min_allowed || *parsed > setting_rule->max_value) {
            finalize_failure("out_of_range", setting_rule->key_name);
            return;
        }

        switch (setting_rule->key_id) {
        case server::core::config::RuntimeSettingKey::kPresenceTtlSec:
            presence_.ttl = *parsed;
            break;
        case server::core::config::RuntimeSettingKey::kRecentHistoryLimit:
            history_.recent_limit = static_cast<std::size_t>(*parsed);
            if (history_.max_list_len < history_.recent_limit) {
                history_.max_list_len = history_.recent_limit;
            }
            break;
        case server::core::config::RuntimeSettingKey::kRoomRecentMaxlen:
            history_.max_list_len = static_cast<std::size_t>(*parsed);
            break;
        case server::core::config::RuntimeSettingKey::kChatSpamThreshold:
            spam_message_threshold_ = static_cast<std::size_t>(*parsed);
            break;
        case server::core::config::RuntimeSettingKey::kChatSpamWindowSec:
            spam_window_sec_ = *parsed;
            break;
        case server::core::config::RuntimeSettingKey::kChatSpamMuteSec:
            spam_mute_sec_ = *parsed;
            break;
        case server::core::config::RuntimeSettingKey::kChatSpamBanSec:
            spam_ban_sec_ = *parsed;
            break;
        case server::core::config::RuntimeSettingKey::kChatSpamBanViolations:
            spam_ban_violation_threshold_ = *parsed;
            break;
        }

        finalize_success(setting_rule->key_name, *parsed);
    })) {
        server::core::runtime_metrics::record_runtime_setting_reload_failure();
        server::core::runtime_metrics::record_runtime_setting_reload_latency(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - request_started_at));
        corelog::warn("admin setting dropped: job queue full");
    }
}

void ChatService::admin_apply_user_moderation(const std::string& op,
                                              const std::vector<std::string>& users,
                                              std::uint32_t duration_sec,
                                              const std::string& reason) {
    if (op.empty() || users.empty()) {
        return;
    }

    const auto trim_ascii_local = [](std::string_view input) {
        std::size_t begin = 0;
        while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
            ++begin;
        }
        std::size_t end = input.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
            --end;
        }
        return std::string(input.substr(begin, end - begin));
    };

    const auto to_lower_ascii_local = [](std::string_view input) {
        std::string out(input);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    };

    std::vector<std::string> targets;
    targets.reserve(users.size());
    for (const auto& user : users) {
        const std::string trimmed = trim_ascii_local(user);
        if (!trimmed.empty()) {
            targets.push_back(trimmed);
        }
    }
    if (targets.empty()) {
        return;
    }

    {
        std::string deny_reason;
        const std::string payload_json =
            std::string("{\"op\":\"") + json_escape(op) +
            "\",\"users\":" + json_array_of_strings(targets) +
            ",\"duration_sec\":" + std::to_string(duration_sec) +
            ",\"reason\":\"" + json_escape(reason) + "\"}";
        if (maybe_handle_admin_command_hook("apply_user_moderation", "control-plane", payload_json, op, deny_reason)) {
            corelog::warn("admin moderation denied by plugin: " + deny_reason);
            return;
        }
    }

    std::string normalized_op = to_lower_ascii_local(trim_ascii_local(op));
    std::string normalized_reason = trim_ascii_local(reason);

    if (!job_queue_.TryPush([this,
                             normalized_op = std::move(normalized_op),
                             targets = std::move(targets),
                             duration_sec,
                             normalized_reason = std::move(normalized_reason)]() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<Session>> affected_sessions;
        std::unordered_set<Session*> seen;

        auto add_user_sessions = [&](const std::string& user) {
            auto itset = state_.by_user.find(user);
            if (itset == state_.by_user.end()) {
                return;
            }
            for (auto wit = itset->second.begin(); wit != itset->second.end();) {
                if (auto session = wit->lock()) {
                    if (seen.insert(session.get()).second) {
                        affected_sessions.push_back(std::move(session));
                    }
                    ++wit;
                } else {
                    wit = itset->second.erase(wit);
                }
            }
        };

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            for (const auto& user : targets) {
                if (normalized_op == "mute") {
                    const std::uint32_t seconds = duration_sec > 0 ? duration_sec : spam_mute_sec_;
                    const std::string applied_reason = normalized_reason.empty() ? "muted by administrator" : normalized_reason;
                    state_.muted_users[user] = {now + std::chrono::seconds(seconds), applied_reason};
                } else if (normalized_op == "unmute") {
                    state_.muted_users.erase(user);
                } else if (normalized_op == "ban") {
                    const std::uint32_t seconds = duration_sec > 0 ? duration_sec : spam_ban_sec_;
                    const auto expires_at = now + std::chrono::seconds(seconds);
                    const std::string applied_reason = normalized_reason.empty() ? "banned by administrator" : normalized_reason;
                    state_.banned_users[user] = {expires_at, applied_reason};

                    if (auto ip_it = state_.user_last_ip.find(user); ip_it != state_.user_last_ip.end() && !ip_it->second.empty()) {
                        state_.banned_ips[ip_it->second] = expires_at;
                    }
                    if (auto hwid_it = state_.user_last_hwid_hash.find(user); hwid_it != state_.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                        state_.banned_hwid_hashes[hwid_it->second] = expires_at;
                    }
                    add_user_sessions(user);
                } else if (normalized_op == "unban") {
                    state_.banned_users.erase(user);
                    if (auto ip_it = state_.user_last_ip.find(user); ip_it != state_.user_last_ip.end() && !ip_it->second.empty()) {
                        state_.banned_ips.erase(ip_it->second);
                    }
                    if (auto hwid_it = state_.user_last_hwid_hash.find(user); hwid_it != state_.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                        state_.banned_hwid_hashes.erase(hwid_it->second);
                    }
                } else if (normalized_op == "kick") {
                    add_user_sessions(user);
                }
            }
        }

        if (normalized_op == "ban" || normalized_op == "kick") {
            const std::string notice = normalized_reason.empty()
                ? (normalized_op == "ban" ? "temporarily banned" : "disconnected by administrator")
                : normalized_reason;
            for (auto& session : affected_sessions) {
                send_system_notice(*session, notice);
                session->stop();
            }
        }

        corelog::info("admin moderation applied: op=" + normalized_op +
                      " requested=" + std::to_string(targets.size()) +
                      " sessions=" + std::to_string(affected_sessions.size()));
    })) {
        corelog::warn("admin moderation dropped: job queue full");
    }
}

std::shared_ptr<ChatService::Session> ChatService::find_session_by_id_locked(std::uint32_t session_id) {
    if (session_id == 0) {
        return {};
    }

    const auto it = state_.by_session_id.find(session_id);
    if (it == state_.by_session_id.end()) {
        return {};
    }

    auto session = it->second.lock();
    if (!session) {
        state_.by_session_id.erase(it);
    }
    return session;
}

std::vector<std::uint8_t> ChatService::make_system_chat_body(std::string_view room, std::string_view text) const {
    server::wire::v1::ChatBroadcast pb;
    pb.set_room(std::string(room));
    pb.set_sender("(system)");
    pb.set_text(std::string(text));
    pb.set_sender_sid(0);
    const auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    pb.set_ts_ms(static_cast<std::uint64_t>(now64));

    std::string bytes;
    pb.SerializeToString(&bytes);
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

bool ChatService::broadcast_notice_to_all_sessions(std::string notice) {
    if (notice.empty()) {
        return false;
    }

    if (!job_queue_.TryPush([this, notice = std::move(notice)]() {
        std::vector<std::shared_ptr<Session>> sessions;
        std::unordered_set<Session*> seen;

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            for (auto& [_, room_set] : state_.rooms) {
                for (auto it = room_set.begin(); it != room_set.end();) {
                    if (auto session = it->lock()) {
                        if (!state_.authed.count(session.get()) || state_.guest.count(session.get())) {
                            ++it;
                            continue;
                        }
                        if (seen.insert(session.get()).second) {
                            sessions.push_back(std::move(session));
                        }
                        ++it;
                    } else {
                        it = room_set.erase(it);
                    }
                }
            }
        }

        for (auto& session : sessions) {
            send_system_notice(*session, notice);
        }
    })) {
        corelog::warn("lua broadcast_all dropped: job queue full");
        return false;
    }
    return true;
}

bool ChatService::apply_user_moderation_without_hook(const std::string& op,
                                                     const std::vector<std::string>& users,
                                                     std::uint32_t duration_sec,
                                                     const std::string& reason) {
    if (op.empty() || users.empty()) {
        return false;
    }

    const auto trim_ascii_local = [](std::string_view input) {
        std::size_t begin = 0;
        while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
            ++begin;
        }
        std::size_t end = input.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
            --end;
        }
        return std::string(input.substr(begin, end - begin));
    };

    const auto to_lower_ascii_local = [](std::string_view input) {
        std::string out(input);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    };

    std::vector<std::string> targets;
    targets.reserve(users.size());
    for (const auto& user : users) {
        const std::string trimmed = trim_ascii_local(user);
        if (!trimmed.empty()) {
            targets.push_back(trimmed);
        }
    }
    if (targets.empty()) {
        return false;
    }

    std::string normalized_op = to_lower_ascii_local(trim_ascii_local(op));
    std::string normalized_reason = trim_ascii_local(reason);

    if (!job_queue_.TryPush([this,
                             normalized_op = std::move(normalized_op),
                             targets = std::move(targets),
                             duration_sec,
                             normalized_reason = std::move(normalized_reason)]() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<Session>> affected_sessions;
        std::unordered_set<Session*> seen;

        auto add_user_sessions = [&](const std::string& user) {
            auto itset = state_.by_user.find(user);
            if (itset == state_.by_user.end()) {
                return;
            }
            for (auto wit = itset->second.begin(); wit != itset->second.end();) {
                if (auto session = wit->lock()) {
                    if (seen.insert(session.get()).second) {
                        affected_sessions.push_back(std::move(session));
                    }
                    ++wit;
                } else {
                    wit = itset->second.erase(wit);
                }
            }
        };

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            for (const auto& user : targets) {
                if (normalized_op == "mute") {
                    const std::uint32_t seconds = duration_sec > 0 ? duration_sec : spam_mute_sec_;
                    const std::string applied_reason = normalized_reason.empty() ? "muted by administrator" : normalized_reason;
                    state_.muted_users[user] = {now + std::chrono::seconds(seconds), applied_reason};
                } else if (normalized_op == "unmute") {
                    state_.muted_users.erase(user);
                } else if (normalized_op == "ban") {
                    const std::uint32_t seconds = duration_sec > 0 ? duration_sec : spam_ban_sec_;
                    const auto expires_at = now + std::chrono::seconds(seconds);
                    const std::string applied_reason = normalized_reason.empty() ? "banned by administrator" : normalized_reason;
                    state_.banned_users[user] = {expires_at, applied_reason};

                    if (auto ip_it = state_.user_last_ip.find(user); ip_it != state_.user_last_ip.end() && !ip_it->second.empty()) {
                        state_.banned_ips[ip_it->second] = expires_at;
                    }
                    if (auto hwid_it = state_.user_last_hwid_hash.find(user); hwid_it != state_.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                        state_.banned_hwid_hashes[hwid_it->second] = expires_at;
                    }
                    add_user_sessions(user);
                } else if (normalized_op == "unban") {
                    state_.banned_users.erase(user);
                    if (auto ip_it = state_.user_last_ip.find(user); ip_it != state_.user_last_ip.end() && !ip_it->second.empty()) {
                        state_.banned_ips.erase(ip_it->second);
                    }
                    if (auto hwid_it = state_.user_last_hwid_hash.find(user); hwid_it != state_.user_last_hwid_hash.end() && !hwid_it->second.empty()) {
                        state_.banned_hwid_hashes.erase(hwid_it->second);
                    }
                } else if (normalized_op == "kick") {
                    add_user_sessions(user);
                }
            }
        }

        if (normalized_op == "ban" || normalized_op == "kick") {
            const std::string notice = normalized_reason.empty()
                ? (normalized_op == "ban" ? "temporarily banned" : "disconnected by administrator")
                : normalized_reason;
            for (auto& session : affected_sessions) {
                send_system_notice(*session, notice);
                session->stop();
            }
        }
    })) {
        corelog::warn("lua moderation dropped: job queue full");
        return false;
    }
    return true;
}

std::optional<std::string> ChatService::lua_get_user_name(std::uint32_t session_id) {
    std::lock_guard<std::mutex> lk(state_.mu);
    auto session = find_session_by_id_locked(session_id);
    if (!session) {
        return std::nullopt;
    }

    const auto it = state_.user.find(session.get());
    if (it == state_.user.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> ChatService::lua_get_user_room(std::uint32_t session_id) {
    std::lock_guard<std::mutex> lk(state_.mu);
    auto session = find_session_by_id_locked(session_id);
    if (!session) {
        return std::nullopt;
    }

    const auto it = state_.cur_room.find(session.get());
    if (it == state_.cur_room.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::string> ChatService::lua_get_room_users(std::string_view room_name) {
    std::vector<std::string> users;
    if (room_name.empty()) {
        return users;
    }

    std::lock_guard<std::mutex> lk(state_.mu);
    const auto it = state_.rooms.find(std::string(room_name));
    if (it == state_.rooms.end()) {
        return users;
    }

    for (auto wit = it->second.begin(); wit != it->second.end();) {
        if (auto session = wit->lock()) {
            if (state_.authed.count(session.get()) > 0 && state_.guest.count(session.get()) == 0) {
                if (const auto user_it = state_.user.find(session.get()); user_it != state_.user.end()) {
                    users.push_back(user_it->second);
                }
            }
            ++wit;
        } else {
            wit = it->second.erase(wit);
        }
    }

    std::sort(users.begin(), users.end());
    users.erase(std::unique(users.begin(), users.end()), users.end());
    return users;
}

std::vector<std::string> ChatService::lua_get_room_list() {
    std::vector<std::string> rooms;
    std::lock_guard<std::mutex> lk(state_.mu);
    rooms.reserve(state_.rooms.size());
    for (const auto& [room_name, _] : state_.rooms) {
        rooms.push_back(room_name);
    }
    std::sort(rooms.begin(), rooms.end());
    return rooms;
}

std::optional<std::string> ChatService::lua_get_room_owner(std::string_view room_name) {
    if (room_name.empty()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lk(state_.mu);
    const auto it = state_.room_owners.find(std::string(room_name));
    if (it == state_.room_owners.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool ChatService::lua_is_user_muted(std::string_view nickname) {
    if (nickname.empty()) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(state_.mu);
    const auto it = state_.muted_users.find(std::string(nickname));
    if (it == state_.muted_users.end()) {
        return false;
    }
    if (it->second.expires_at <= now) {
        state_.muted_users.erase(it);
        return false;
    }
    return true;
}

bool ChatService::lua_is_user_banned(std::string_view nickname) {
    if (nickname.empty()) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(state_.mu);
    const auto it = state_.banned_users.find(std::string(nickname));
    if (it == state_.banned_users.end()) {
        return false;
    }
    if (it->second.expires_at <= now) {
        state_.banned_users.erase(it);
        return false;
    }
    return true;
}

std::size_t ChatService::lua_get_online_count() {
    std::unordered_set<std::string> names;
    std::lock_guard<std::mutex> lk(state_.mu);
    for (const auto& [session, user] : state_.user) {
        if (state_.authed.count(session) == 0 || state_.guest.count(session) > 0) {
            continue;
        }
        if (!user.empty()) {
            names.insert(user);
        }
    }
    return names.size();
}

std::size_t ChatService::lua_get_room_count() {
    std::lock_guard<std::mutex> lk(state_.mu);
    return state_.rooms.size();
}

bool ChatService::lua_send_notice(std::uint32_t session_id, std::string_view text) {
    if (session_id == 0 || text.empty()) {
        return false;
    }

    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        session = find_session_by_id_locked(session_id);
    }
    if (!session) {
        return false;
    }

    send_system_notice(*session, std::string(text));
    return true;
}

bool ChatService::lua_broadcast_room(std::string_view room_name, std::string_view text) {
    if (room_name.empty() || text.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (state_.rooms.find(std::string(room_name)) == state_.rooms.end()) {
            return false;
        }
    }

    broadcast_room(std::string(room_name), make_system_chat_body(room_name, text), nullptr);
    return true;
}

bool ChatService::lua_broadcast_all(std::string_view text) {
    if (text.empty()) {
        return false;
    }

    return broadcast_notice_to_all_sessions(std::string(text));
}

bool ChatService::lua_kick_user(std::uint32_t session_id, std::string_view reason) {
    if (session_id == 0) {
        return false;
    }

    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        session = find_session_by_id_locked(session_id);
    }
    if (!session) {
        return false;
    }

    if (!reason.empty()) {
        send_system_notice(*session, std::string(reason));
    }
    session->stop();
    return true;
}

bool ChatService::lua_mute_user(std::string_view nickname,
                                std::uint32_t duration_sec,
                                std::string_view reason) {
    if (nickname.empty()) {
        return false;
    }

    return apply_user_moderation_without_hook(
        "mute",
        {std::string(nickname)},
        duration_sec,
        std::string(reason));
}

bool ChatService::lua_ban_user(std::string_view nickname,
                               std::uint32_t duration_sec,
                               std::string_view reason) {
    if (nickname.empty()) {
        return false;
    }

    return apply_user_moderation_without_hook(
        "ban",
        {std::string(nickname)},
        duration_sec,
        std::string(reason));
}

// 귓속말 전송 결과를 클라이언트에게 알립니다.
void ChatService::send_whisper_result(Session& s, bool ok, const std::string& reason) {
    server::wire::v1::WhisperResult pb;
    pb.set_ok(ok);
    if (!reason.empty()) {
        pb.set_reason(reason);
    }
    std::string bytes;
    pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
    s.async_send(game_proto::MSG_WHISPER_RES, body, 0);
}

void ChatService::deliver_remote_whisper(const std::vector<std::uint8_t>& body) {
    if (body.empty()) {
        return;
    }

    server::wire::v1::WhisperNotice notice;
    if (!notice.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        corelog::warn("[whisper] failed to parse remote payload");
        return;
    }

    if (notice.sender().empty() || notice.recipient().empty() || notice.text().empty()) {
        corelog::warn("[whisper] invalid remote payload fields");
        return;
    }

    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (auto blk_it = state_.user_blacklists.find(notice.recipient());
            blk_it != state_.user_blacklists.end() && blk_it->second.count(notice.sender()) > 0) {
            return;
        }
        auto itset = state_.by_user.find(notice.recipient());
        if (itset != state_.by_user.end()) {
            for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                if (auto p = wit->lock()) {
                    if (!state_.authed.count(p.get()) || state_.guest.count(p.get())) {
                        ++wit;
                        continue;
                    }
                    targets.emplace_back(std::move(p));
                    ++wit;
                } else {
                    wit = itset->second.erase(wit);
                }
            }
        }
    }

    if (targets.empty()) {
        return;
    }

    notice.set_outgoing(false);
    std::string incoming_bytes;
    if (!notice.SerializeToString(&incoming_bytes)) {
        return;
    }
    std::vector<std::uint8_t> incoming(incoming_bytes.begin(), incoming_bytes.end());
    for (auto& target : targets) {
        target->async_send(game_proto::MSG_WHISPER_BROADCAST, incoming, 0);
    }

    corelog::debug("[whisper] sender=" + notice.sender() +
                   " target=" + notice.recipient() +
                   " status=remote_delivered count=" + std::to_string(targets.size()));
}

// 귓속말(1:1 채팅)을 처리합니다.
// 대상 사용자가 같은 서버에 있으면 직접 전송하고, 없으면 Redis Pub/Sub을 통해 전파할 수도 있습니다(현재 구현은 로컬 우선).
void ChatService::dispatch_whisper(std::shared_ptr<Session> session_sp, const std::string& target_user, const std::string& text) {
    if (!session_sp) return;
    if (target_user.empty() || text.empty()) {
        send_system_notice(*session_sp, "usage: /whisper <user> <message>");
        send_whisper_result(*session_sp, false, "invalid payload");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (!state_.authed.count(session_sp.get())) {
            session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized");
            return;
        }
    }

    std::string sender;
    bool sender_is_guest = false;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it_sender = state_.user.find(session_sp.get());
        sender = (it_sender != state_.user.end()) ? it_sender->second : std::string("guest");
        sender_is_guest = state_.guest.count(session_sp.get()) > 0;
    }
    if (sender == "guest" || sender_is_guest) {
        send_system_notice(*session_sp, "login required for whisper");
        send_whisper_result(*session_sp, false, "login required");
        session_sp->send_error(proto::errc::UNAUTHORIZED, "guest cannot whisper");
        return;
    }

    if (target_user == sender) {
        send_system_notice(*session_sp, "cannot whisper to yourself");
        send_whisper_result(*session_sp, false, "cannot whisper to yourself");
        corelog::debug("[whisper] sender=" + sender + " target=" + target_user + " status=self_target");
        return;
    }

    std::vector<std::shared_ptr<Session>> targets;
    bool ineligible_found = false;
    bool blocked_by_sender = false;
    bool blocked_by_target = false;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (auto sender_blk_it = state_.user_blacklists.find(sender);
            sender_blk_it != state_.user_blacklists.end() && sender_blk_it->second.count(target_user) > 0) {
            blocked_by_sender = true;
        }
        if (auto target_blk_it = state_.user_blacklists.find(target_user);
            target_blk_it != state_.user_blacklists.end() && target_blk_it->second.count(sender) > 0) {
            blocked_by_target = true;
        }

        if (blocked_by_sender || blocked_by_target) {
            // behave similar to offline/hidden target for privacy
            ineligible_found = true;
        }

        auto itset = state_.by_user.find(target_user);
        if (itset != state_.by_user.end()) {
            for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                if (auto p = wit->lock()) {
                    if (p.get() == session_sp.get()) {
                        ++wit;
                        continue;
                    }
                    if (!state_.authed.count(p.get()) || state_.guest.count(p.get())) {
                        ineligible_found = true;
                        ++wit;
                        continue;
                    }
                    if (blocked_by_sender || blocked_by_target) {
                        ineligible_found = true;
                        ++wit;
                        continue;
                    }
                    if (p.get() != session_sp.get()) {
                        targets.emplace_back(std::move(p));
                    }
                    ++wit;
                } else {
                    wit = itset->second.erase(wit);
                }
            }
        }
    }

    if (targets.empty() && ineligible_found) {
        if (blocked_by_sender) {
            send_system_notice(*session_sp, "whisper denied: unblock target first");
            send_whisper_result(*session_sp, false, "recipient blocked by sender");
            corelog::debug("[whisper] sender=" + sender + " target=" + target_user + " status=blocked_by_sender");
            return;
        }
        send_system_notice(*session_sp, "user cannot receive whispers (login required): " + target_user);
        send_whisper_result(*session_sp, false, "recipient not eligible");
        corelog::debug("[whisper] sender=" + sender + " target=" + target_user + " status=recipient_guest_or_blocked");
        return;
    }

    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    server::wire::v1::WhisperNotice notice;
    notice.set_sender(sender);
    notice.set_recipient(target_user);
    notice.set_text(text);
    notice.set_ts_ms(static_cast<std::uint64_t>(now64));

    const auto send_outgoing = [&]() {
        notice.set_outgoing(true);
        std::string outgoing_bytes;
        notice.SerializeToString(&outgoing_bytes);
        std::vector<std::uint8_t> outgoing(outgoing_bytes.begin(), outgoing_bytes.end());
        session_sp->async_send(game_proto::MSG_WHISPER_BROADCAST, outgoing, 0);
    };

    if (targets.empty()) {
        bool routed_remote = false;
        if (redis_ && pubsub_enabled()) {
            try {
                notice.set_outgoing(false);
                std::string incoming_bytes;
                if (notice.SerializeToString(&incoming_bytes)) {
                    std::string message;
                    message.reserve(3 + gateway_id_.size() + 1 + incoming_bytes.size());
                    message.append("gw=").append(gateway_id_);
                    message.push_back('\n');
                    message.append(incoming_bytes);
                    const std::string channel = presence_.prefix + std::string("fanout:whisper");
                    routed_remote = redis_->publish(channel, std::move(message));
                }
            } catch (...) {}
        }

        if (!routed_remote) {
            send_system_notice(*session_sp, "user not found: " + target_user);
            send_whisper_result(*session_sp, false, "user not found");
            corelog::debug("[whisper] sender=" + sender + " target=" + target_user + " status=not_found");
            return;
        }

        send_outgoing();
        send_whisper_result(*session_sp, true, "");
        corelog::debug("[whisper] sender=" + sender + " target=" + target_user + " status=remote_routed");
        return;
    }

    notice.set_outgoing(false);
    std::string incoming_bytes;
    notice.SerializeToString(&incoming_bytes);
    std::vector<std::uint8_t> incoming(incoming_bytes.begin(), incoming_bytes.end());
    for (auto& target : targets) {
        target->async_send(game_proto::MSG_WHISPER_BROADCAST, incoming, 0);
    }

    send_outgoing();

    send_whisper_result(*session_sp, true, "");
}

// 방 비밀번호 해싱 (versioned format)
std::string ChatService::hash_room_password(const std::string& password) {
    std::string digest = sha256_hex(password);
    if (digest.empty()) {
        return {};
    }
    return std::string(kRoomPasswordHashPrefix) + digest;
}

std::string ChatService::hash_hwid_token(std::string_view token) const {
    if (token.empty()) {
        return {};
    }
    std::string digest = sha256_hex(token);
    if (digest.empty()) {
        return {};
    }
    return std::string("hwid:") + digest;
}

bool ChatService::verify_room_password(const std::string& password, const std::string& stored_hash) {
    if (stored_hash.empty()) {
        return false;
    }

    if (has_room_password_hash_prefix(stored_hash)) {
        return hash_room_password(password) == stored_hash;
    }

    // Backward compatibility for legacy hashes.
    return legacy_hash_room_password(password) == stored_hash;
}

bool ChatService::is_modern_room_password_hash(const std::string& stored_hash) const {
    return has_room_password_hash_prefix(stored_hash);
}

// 방 이름으로 Room ID(UUID)를 조회하거나 생성합니다. (Case-Insensitive)
std::string ChatService::ensure_room_id_ci(const std::string& room_name) {
    if (!db_pool_) return std::string();
    // 캐시 확인
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.room_ids.find(room_name);
        if (it != state_.room_ids.end()) return it->second;
    }
    try {
        auto uow = db_pool_->make_repository_unit_of_work();
        auto found = uow->rooms().find_by_name_exact_ci(room_name);
        std::string id;
        if (found) {
            id = found->id;
        } else {
            auto created = uow->rooms().create(room_name, true);
            id = created.id;
        }
        uow->commit();
        std::lock_guard<std::mutex> lk2(state_.mu);
        state_.room_ids.emplace(room_name, id);
        return id;
    } catch (const std::exception& e) {
        corelog::error(std::string("ensure_room_id_ci failed: ") + e.what());
        return std::string();
    }
}


std::string ChatService::make_recent_list_key(const std::string& room_id) const {
    return std::string("room:") + room_id + ":recent";
}

std::string ChatService::make_recent_message_key(std::uint64_t message_id) const {
    return std::string("msg:") + std::to_string(message_id);
}

// 최근 메시지를 Redis 캐시에 저장합니다. (LIST + Key-Value)
bool ChatService::cache_recent_message(
    const std::string& room_id,
    const server::wire::v1::StateSnapshot::SnapshotMessage& message) {
    if (!redis_ || room_id.empty() || message.id() == 0) {
        return false;
    }
    std::string payload;
    if (!message.SerializeToString(&payload)) {
        return false;
    }
    if (!redis_->setex(make_recent_message_key(message.id()), payload, history_.cache_ttl_sec)) {
        return false;
    }
    return redis_->lpush_trim(make_recent_list_key(room_id),
                              std::to_string(message.id()),
                              history_.max_list_len);
}

// Redis 캐시에서 최근 메시지 목록을 로드합니다.
bool ChatService::load_recent_messages_from_cache(
    const std::string& room_id,
    std::vector<server::wire::v1::StateSnapshot::SnapshotMessage>& out) {
    if (!redis_ || room_id.empty() || history_.recent_limit == 0) {
        return false;
    }
    std::vector<std::string> ids;
    // LPUSH를 사용하므로 리스트의 앞부분(0)이 가장 최신 메시지입니다.
    // 따라서 최신 N개를 가져오려면 0부터 N-1까지 읽어야 합니다.
    // (기존 코드는 -limit ~ -1로 읽어서 가장 오래된 메시지를 가져오는 버그가 있었음)
    if (!redis_->lrange(make_recent_list_key(room_id), 0, static_cast<long long>(history_.recent_limit) - 1, ids)) {
        return false;
    }
    if (ids.empty()) {
        return false;
    }

    std::vector<std::uint64_t> valid_ids;
    valid_ids.reserve(ids.size());

    bool partial_hit = false;
    for (const auto& id_str : ids) {
        char* endptr = nullptr;
        auto value = std::strtoull(id_str.c_str(), &endptr, 10);
        if (endptr == id_str.c_str() || value == 0) {
            partial_hit = true;
            corelog::warn("recent history cache miss: invalid id entry=" + id_str);
            continue;
        }
        valid_ids.emplace_back(value);
    }

    if (valid_ids.empty()) {
        return false;
    }

    std::vector<server::wire::v1::StateSnapshot::SnapshotMessage> parsed;
    parsed.reserve(valid_ids.size());

    bool batch_loaded = false;
    {
        std::vector<std::string> keys;
        keys.reserve(valid_ids.size());
        for (const auto id : valid_ids) {
            keys.emplace_back(make_recent_message_key(id));
        }

        std::vector<std::optional<std::string>> payloads;
        if (redis_->mget(keys, payloads) && payloads.size() == keys.size()) {
            batch_loaded = true;
            for (std::size_t i = 0; i < payloads.size(); ++i) {
                if (!payloads[i].has_value()) {
                    partial_hit = true;
                    continue;
                }
                server::wire::v1::StateSnapshot::SnapshotMessage message;
                if (!message.ParseFromString(*payloads[i])) {
                    partial_hit = true;
                    corelog::warn("recent history cache miss: corrupted payload");
                    continue;
                }
                parsed.emplace_back(std::move(message));
            }
        }
    }

    if (!batch_loaded) {
        for (const auto id : valid_ids) {
            auto payload = redis_->get(make_recent_message_key(id));
            if (!payload) {
                partial_hit = true;
                continue;
            }
            server::wire::v1::StateSnapshot::SnapshotMessage message;
            if (!message.ParseFromString(*payload)) {
                partial_hit = true;
                corelog::warn("recent history cache miss: corrupted payload");
                continue;
            }
            parsed.emplace_back(std::move(message));
        }
    }
    if (parsed.empty()) {
        return false;
    }
    if (partial_hit) {
        corelog::info("recent history cache partial hit: room_id=" + room_id +
                      " kept=" + std::to_string(parsed.size()) +
                      " requested=" + std::to_string(ids.size()));
    }
    std::reverse(parsed.begin(), parsed.end());
    out = std::move(parsed);
    return true;
}

} // namespace server::app::chat
