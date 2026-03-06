#pragma once

#include "server/core/scripting/lua_sandbox.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#ifndef KNIGHTS_BUILD_LUA_SCRIPTING
#define KNIGHTS_BUILD_LUA_SCRIPTING 0
#endif

namespace server::core::scripting {

/** @brief Hook invocation metadata exposed to Lua functions and host callbacks. */
struct LuaHookContext {
    std::uint32_t session_id{0};
    std::string user;
    std::string room;
    std::string text;
    std::string command;
    std::string args;
    std::string issuer;
    std::string reason;
    std::string payload_json;
    std::string event;
};

enum class LuaHookDecision {
    kPass,
    kHandled,
    kBlock,
    kModify,
    kAllow,
    kDeny,
};

/**
 * @brief Build-toggle-safe Lua runtime facade used by server-side scripting hooks.
 *
 * When `KNIGHTS_BUILD_LUA_SCRIPTING=1`, the runtime follows the enabled implementation path.
 * When `KNIGHTS_BUILD_LUA_SCRIPTING=0`, the runtime keeps the same API surface and returns
 * deterministic disabled-mode results so callers can preserve control-flow compatibility.
 */
class LuaRuntime {
public:
    enum class ScriptFailureKind {
        kNone,
        kInstructionLimit,
        kMemoryLimit,
        kOther,
    };

    /** @brief Runtime policy and limit configuration. */
    struct Config {
        std::uint64_t instruction_limit{100'000};
        std::size_t memory_limit_bytes{1 * 1024 * 1024};
        std::vector<std::string> allowed_libraries{
            "base",
            "string",
            "table",
            "math",
            "utf8",
        };
    };

    /** @brief Result of a single script load operation. */
    struct LoadResult {
        bool ok{false};
        std::string error;
    };

    /** @brief Result of a single-environment hook call. */
    struct CallResult {
        bool ok{false};
        bool executed{false};
        std::string error;
    };

    /** @brief Result of a multi-environment hook call. */
    struct CallAllResult {
        /** @brief Per-script execution status within a call_all dispatch. */
        struct ScriptCallResult {
            std::string env_name;
            bool failed{false};
            ScriptFailureKind failure_kind{ScriptFailureKind::kNone};
        };

        std::size_t attempted{0};
        std::size_t failed{0};
        LuaHookDecision decision{LuaHookDecision::kPass};
        std::string reason;
        std::vector<std::string> notices;
        std::string error;
        std::vector<ScriptCallResult> script_results;
    };

    /** @brief Script registration entry used by reload operations. */
    struct ScriptEntry {
        std::filesystem::path path;
        std::string env_name;
    };

    /** @brief Host API value exchanged between Lua and C++ binding callbacks. */
    struct HostValue {
        using StringList = std::vector<std::string>;
        using Storage = std::variant<std::monostate, bool, std::int64_t, std::string, StringList>;

        Storage value{};

        HostValue() = default;
        HostValue(bool v) : value(v) {}
        HostValue(std::int64_t v) : value(v) {}
        HostValue(std::string v) : value(std::move(v)) {}
        HostValue(const char* v) : value(std::string(v ? v : "")) {}
        HostValue(StringList v) : value(std::move(v)) {}

        [[nodiscard]] bool is_nil() const {
            return std::holds_alternative<std::monostate>(value);
        }
    };

    /** @brief Per-host-call metadata with runtime/script annotations. */
    struct HostCallContext {
        std::string hook_name;
        std::string script_name;
        LuaHookContext hook;
    };

    /** @brief Host callback return payload or error. */
    struct HostCallResult {
        HostValue value{};
        std::string error;
    };

    /** @brief Result of replacing active runtime scripts. */
    struct ReloadResult {
        std::size_t loaded{0};
        std::size_t failed{0};
        std::string error;
    };

    using HostArgs = std::vector<HostValue>;
    using HostCallback = std::function<HostCallResult(const HostArgs&, const HostCallContext&)>;

    /** @brief Point-in-time runtime counters and gauges. */
    struct MetricsSnapshot {
        std::size_t loaded_scripts{0};
        std::size_t registered_host_api{0};
        std::size_t memory_used_bytes{0};
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
        std::uint64_t instruction_limit_hits{0};
        std::uint64_t memory_limit_hits{0};
        std::uint64_t reload_epoch{0};
    };

    LuaRuntime();
    explicit LuaRuntime(Config cfg);

    LoadResult load_script(const std::filesystem::path& path, const std::string& env_name);
    ReloadResult reload_scripts(const std::vector<ScriptEntry>& scripts);
    CallResult call(const std::string& env_name,
                    const std::string& func_name,
                    const LuaHookContext& ctx = {});
    CallAllResult call_all(const std::string& func_name,
                           const LuaHookContext& ctx = {});
    bool register_host_api(const std::string& table_name,
                           const std::string& func_name,
                           HostCallback callback);
    void reset();
    bool enabled() const;
    MetricsSnapshot metrics_snapshot() const;

private:
    static std::string make_api_key(std::string_view table_name, std::string_view func_name);

    mutable std::mutex mu_;
    Config cfg_;
    sandbox::Policy policy_;
    std::unordered_map<std::string, std::filesystem::path> loaded_scripts_;
    std::unordered_map<std::string, HostCallback> host_api_;
    std::size_t memory_used_bytes_{0};
    std::uint64_t calls_total_{0};
    std::uint64_t errors_total_{0};
    std::uint64_t instruction_limit_hits_{0};
    std::uint64_t memory_limit_hits_{0};
    std::uint64_t reload_epoch_{0};
};

} // namespace server::core::scripting
