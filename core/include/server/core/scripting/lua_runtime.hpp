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
#include <vector>

#ifndef KNIGHTS_BUILD_LUA_SCRIPTING
#define KNIGHTS_BUILD_LUA_SCRIPTING 0
#endif

namespace server::core::scripting {

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

    /** @brief Result of replacing active runtime scripts. */
    struct ReloadResult {
        std::size_t loaded{0};
        std::size_t failed{0};
        std::string error;
    };

    using HostCallback = std::function<void()>;

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

    /**
     * @brief Construct a runtime with default configuration.
     */
    LuaRuntime();

    /**
     * @brief Construct a runtime with custom configuration.
     * @param cfg Runtime configuration values applied to sandbox policy and limits.
     */
    explicit LuaRuntime(Config cfg);

    /**
     * @brief Load one script into a named runtime environment.
     * @param path Script file path.
     * @param env_name Logical environment key for later dispatch.
     * @return Load status and optional error message.
     */
    LoadResult load_script(const std::filesystem::path& path, const std::string& env_name);

    /**
     * @brief Replace tracked script set with the provided entries.
     * @param scripts Script entries to load as the new active set.
     * @return Reload summary including loaded/failed counts and optional error message.
     */
    ReloadResult reload_scripts(const std::vector<ScriptEntry>& scripts);

    /**
     * @brief Call one hook function for a specific environment.
     * @param env_name Environment key.
     * @param func_name Hook function name.
     * @return Call result with execution status and optional error message.
     */
    CallResult call(const std::string& env_name, const std::string& func_name);

    /**
     * @brief Call one hook function for all currently loaded environments.
     * @param func_name Hook function name.
     * @return Aggregated multi-script call result.
     */
    CallAllResult call_all(const std::string& func_name);

    /**
     * @brief Register a host callback under `table_name.func_name`.
     * @param table_name Host table namespace.
     * @param func_name Function name within the host table.
     * @param callback Callback invoked from runtime integration points.
     * @return `true` when registration succeeds, otherwise `false`.
     */
    bool register_host_api(const std::string& table_name,
                           const std::string& func_name,
                           HostCallback callback);

    /**
     * @brief Reset runtime state and counters.
     */
    void reset();

    /**
     * @brief Report whether runtime scripting execution is enabled for this build.
     * @return `true` when scripting path is enabled, otherwise `false`.
     */
    bool enabled() const;

    /**
     * @brief Return a point-in-time snapshot of runtime metrics.
     * @return Current runtime metrics snapshot.
     */
    MetricsSnapshot metrics_snapshot() const;

private:
    static std::string make_api_key(std::string_view table_name, std::string_view func_name);

    mutable std::mutex mu_;
    Config cfg_;
    sandbox::Policy policy_;
    std::unordered_map<std::string, std::filesystem::path> loaded_scripts_;
    std::unordered_map<std::string, HostCallback> host_api_;
    std::uint64_t calls_total_{0};
    std::uint64_t errors_total_{0};
    std::uint64_t instruction_limit_hits_{0};
    std::uint64_t memory_limit_hits_{0};
    std::uint64_t reload_epoch_{0};
};

} // namespace server::core::scripting
