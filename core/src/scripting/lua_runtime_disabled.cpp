#include "server/core/scripting/lua_runtime.hpp"

#include <utility>

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

LuaRuntime::LoadResult LuaRuntime::load_script(const std::filesystem::path&,
                                               const std::string&) {
    std::lock_guard<std::mutex> lock(mu_);
    ++errors_total_;
    return LoadResult{false, make_disabled_error()};
}

LuaRuntime::ReloadResult LuaRuntime::reload_scripts(const std::vector<ScriptEntry>& scripts) {
    std::lock_guard<std::mutex> lock(mu_);
    ++errors_total_;
    return ReloadResult{0, scripts.size(), make_disabled_error()};
}

LuaRuntime::CallResult LuaRuntime::call(const std::string&,
                                        const std::string&,
                                        const LuaHookContext&) {
    std::lock_guard<std::mutex> lock(mu_);
    ++errors_total_;
    return CallResult{false, false, make_disabled_error()};
}

LuaRuntime::CallAllResult LuaRuntime::call_all(const std::string&,
                                               const LuaHookContext&) {
    std::lock_guard<std::mutex> lock(mu_);
    ++errors_total_;
    return CallAllResult{
        0,
        loaded_scripts_.size(),
        LuaHookDecision::kPass,
        {},
        {},
        make_disabled_error(),
        {},
    };
}

bool LuaRuntime::register_host_api(const std::string&,
                                   const std::string&,
                                   HostCallback) {
    std::lock_guard<std::mutex> lock(mu_);
    ++errors_total_;
    return false;
}

void LuaRuntime::reset() {
    std::lock_guard<std::mutex> lock(mu_);

    loaded_scripts_.clear();
    host_api_.clear();
    memory_used_bytes_ = 0;
    calls_total_ = 0;
    errors_total_ = 0;
    instruction_limit_hits_ = 0;
    memory_limit_hits_ = 0;
    reload_epoch_ = 0;
}

bool LuaRuntime::enabled() const {
    return false;
}

LuaRuntime::MetricsSnapshot LuaRuntime::metrics_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);

    MetricsSnapshot snapshot{};
    snapshot.loaded_scripts = loaded_scripts_.size();
    snapshot.registered_host_api = host_api_.size();
    snapshot.memory_used_bytes = memory_used_bytes_;
    snapshot.calls_total = calls_total_;
    snapshot.errors_total = errors_total_;
    snapshot.instruction_limit_hits = instruction_limit_hits_;
    snapshot.memory_limit_hits = memory_limit_hits_;
    snapshot.reload_epoch = reload_epoch_;
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
