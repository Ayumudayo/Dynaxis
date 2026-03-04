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

/**
 * @brief Build-toggle-safe Lua runtime scaffold for Stream B.
 */
class LuaRuntime {
public:
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

    struct LoadResult {
        bool ok{false};
        std::string error;
    };

    struct CallResult {
        bool ok{false};
        bool executed{false};
        std::string error;
    };

    struct ScriptEntry {
        std::filesystem::path path;
        std::string env_name;
    };

    struct ReloadResult {
        std::size_t loaded{0};
        std::size_t failed{0};
        std::string error;
    };

    using HostCallback = std::function<void()>;

    struct MetricsSnapshot {
        std::size_t loaded_scripts{0};
        std::size_t registered_host_api{0};
        std::size_t memory_used_bytes{0};
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
        std::uint64_t instruction_limit_hits{0};
        std::uint64_t memory_limit_hits{0};
    };

    LuaRuntime();

    explicit LuaRuntime(Config cfg);

    LoadResult load_script(const std::filesystem::path& path, const std::string& env_name);

    ReloadResult reload_scripts(const std::vector<ScriptEntry>& scripts);

    CallResult call(const std::string& env_name, const std::string& func_name);

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
    std::uint64_t calls_total_{0};
    std::uint64_t errors_total_{0};
    std::uint64_t instruction_limit_hits_{0};
    std::uint64_t memory_limit_hits_{0};
};

} // namespace server::core::scripting
