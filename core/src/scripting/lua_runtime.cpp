#include "server/core/scripting/lua_runtime.hpp"

#include <system_error>

namespace server::core::scripting {

namespace {

std::string make_disabled_error() {
    return "lua scripting is disabled at build time (BUILD_LUA_SCRIPTING=OFF)";
}

} // namespace

LuaRuntime::LuaRuntime()
    : LuaRuntime(Config{}) {
}

LuaRuntime::LuaRuntime(Config cfg)
    : cfg_(std::move(cfg)),
      policy_(sandbox::default_policy()) {
    policy_.instruction_limit = cfg_.instruction_limit;
    policy_.memory_limit_bytes = cfg_.memory_limit_bytes;
    policy_.allowed_libraries = cfg_.allowed_libraries;
}

LuaRuntime::LoadResult LuaRuntime::load_script(const std::filesystem::path& path,
                                               const std::string& env_name) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return LoadResult{false, make_disabled_error()};
    }

    if (env_name.empty()) {
        ++errors_total_;
        return LoadResult{false, "env_name is empty"};
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        ++errors_total_;
        return LoadResult{false, "script file does not exist"};
    }

    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        ++errors_total_;
        return LoadResult{false, "script path is not a regular file"};
    }

    loaded_scripts_[env_name] = path;
    return LoadResult{true, {}};
}

LuaRuntime::ReloadResult LuaRuntime::reload_scripts(const std::vector<ScriptEntry>& scripts) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return ReloadResult{0, scripts.size(), make_disabled_error()};
    }

    std::unordered_map<std::string, std::filesystem::path> reloaded;
    reloaded.reserve(scripts.size());

    std::size_t failed = 0;
    for (const auto& script : scripts) {
        if (script.env_name.empty()) {
            ++errors_total_;
            ++failed;
            continue;
        }

        std::error_code ec;
        if (!std::filesystem::exists(script.path, ec) || ec) {
            ++errors_total_;
            ++failed;
            continue;
        }
        if (!std::filesystem::is_regular_file(script.path, ec) || ec) {
            ++errors_total_;
            ++failed;
            continue;
        }

        auto [it, inserted] = reloaded.emplace(script.env_name, script.path);
        if (!inserted) {
            ++errors_total_;
            ++failed;
            it->second = script.path;
        }
    }

    loaded_scripts_ = std::move(reloaded);
    return ReloadResult{loaded_scripts_.size(), failed, {}};
}

LuaRuntime::CallResult LuaRuntime::call(const std::string& env_name,
                                        const std::string& func_name) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return CallResult{false, false, make_disabled_error()};
    }

    if (func_name.empty()) {
        ++errors_total_;
        return CallResult{false, false, "func_name is empty"};
    }

    const auto it = loaded_scripts_.find(env_name);
    if (it == loaded_scripts_.end()) {
        return CallResult{true, false, {}};
    }

    (void)it;
    ++calls_total_;
    return CallResult{true, false, {}};
}

bool LuaRuntime::register_host_api(const std::string& table_name,
                                   const std::string& func_name,
                                   HostCallback callback) {
    std::lock_guard<std::mutex> lock(mu_);

    if (!enabled()) {
        ++errors_total_;
        return false;
    }

    if (table_name.empty() || func_name.empty() || !callback) {
        ++errors_total_;
        return false;
    }

    host_api_[make_api_key(table_name, func_name)] = std::move(callback);
    return true;
}

void LuaRuntime::reset() {
    std::lock_guard<std::mutex> lock(mu_);

    loaded_scripts_.clear();
    host_api_.clear();
    calls_total_ = 0;
    errors_total_ = 0;
    instruction_limit_hits_ = 0;
    memory_limit_hits_ = 0;
}

bool LuaRuntime::enabled() const {
#if KNIGHTS_BUILD_LUA_SCRIPTING
    return true;
#else
    return false;
#endif
}

LuaRuntime::MetricsSnapshot LuaRuntime::metrics_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);

    MetricsSnapshot snapshot{};
    snapshot.loaded_scripts = loaded_scripts_.size();
    snapshot.registered_host_api = host_api_.size();
    snapshot.memory_used_bytes = 0;
    snapshot.calls_total = calls_total_;
    snapshot.errors_total = errors_total_;
    snapshot.instruction_limit_hits = instruction_limit_hits_;
    snapshot.memory_limit_hits = memory_limit_hits_;
    return snapshot;
}

std::string LuaRuntime::make_api_key(std::string_view table_name,
                                     std::string_view func_name) {
    std::string key;
    key.reserve(table_name.size() + func_name.size() + 1);
    key.append(table_name);
    key.push_back('.');
    key.append(func_name);
    return key;
}

} // namespace server::core::scripting
