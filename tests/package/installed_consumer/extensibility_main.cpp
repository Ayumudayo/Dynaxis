#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "server/core/plugin/plugin_chain_host.hpp"
#include "server/core/plugin/plugin_host.hpp"
#include "server/core/plugin/shared_library.hpp"
#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/scripting/lua_sandbox.hpp"
#include "server/core/scripting/script_watcher.hpp"

namespace {

struct InstalledConsumerPluginApi {
    std::uint32_t abi_version{1};
    const char* name{"installed_consumer"};
};

bool require_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path make_temp_dir(const std::string& tag) {
    const auto base = std::filesystem::temp_directory_path();
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = base / (tag + "_" + std::to_string(nonce));
    std::error_code ec;
    (void)std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
}

} // namespace

int main() {
    static_assert(sizeof(server::core::plugin::SharedLibrary) >= sizeof(void*));

    server::core::plugin::PluginHost<InstalledConsumerPluginApi>::Config host_cfg{};
    host_cfg.plugin_path = std::filesystem::path{"consumer_plugin.dll"};
    host_cfg.cache_dir = std::filesystem::temp_directory_path() / "server_core_installed_consumer_cache";
    host_cfg.entrypoint_symbol = "consumer_plugin_api_v1";
    host_cfg.api_resolver = [](void*, std::string&) -> const InstalledConsumerPluginApi* { return nullptr; };
    host_cfg.api_validator = [](const InstalledConsumerPluginApi*, std::string&) { return true; };
    host_cfg.instance_creator = [](const InstalledConsumerPluginApi*, std::string&) -> void* { return nullptr; };
    host_cfg.instance_destroyer = [](const InstalledConsumerPluginApi*, void*) {};
    server::core::plugin::PluginHost<InstalledConsumerPluginApi>::MetricsSnapshot host_metrics{};
    (void)host_metrics.loaded;

    server::core::plugin::PluginChainHost<InstalledConsumerPluginApi>::Config chain_cfg{};
    chain_cfg.plugins_dir = std::filesystem::temp_directory_path() / "server_core_installed_consumer_plugins";
    chain_cfg.cache_dir = std::filesystem::temp_directory_path() / "server_core_installed_consumer_chain_cache";
    chain_cfg.entrypoint_symbol = "consumer_plugin_api_v1";
    chain_cfg.api_resolver = [](void*, std::string&) -> const InstalledConsumerPluginApi* { return nullptr; };
    chain_cfg.api_validator = [](const InstalledConsumerPluginApi*, std::string&) { return true; };
    chain_cfg.instance_creator = [](const InstalledConsumerPluginApi*, std::string&) -> void* { return nullptr; };
    chain_cfg.instance_destroyer = [](const InstalledConsumerPluginApi*, void*) {};
    server::core::plugin::SharedLibrary library;
    server::core::plugin::PluginHost<InstalledConsumerPluginApi> plugin_host(host_cfg);
    server::core::plugin::PluginChainHost<InstalledConsumerPluginApi> plugin_chain(chain_cfg);
    server::core::plugin::PluginChainHost<InstalledConsumerPluginApi>::MetricsSnapshot chain_metrics{};
    (void)chain_metrics.configured;
    (void)library.is_loaded();
    (void)plugin_host.metrics_snapshot();
    (void)plugin_chain.metrics_snapshot();

    const auto temp_dir = make_temp_dir("server_core_installed_extensibility_consumer");
    const auto script_path = temp_dir / "policy.lua";
    write_text(script_path, "return { hook = \"on_login\", decision = \"deny\", reason = \"installed deny\" }\n");

    server::core::scripting::ScriptWatcher::Config watcher_cfg{};
    watcher_cfg.scripts_dir = temp_dir;
    watcher_cfg.extensions = {".lua"};
    server::core::scripting::ScriptWatcher watcher(watcher_cfg);

    server::core::scripting::sandbox::Policy sandbox_policy = server::core::scripting::sandbox::default_policy();

    server::core::scripting::LuaRuntime::Config runtime_cfg{};
    runtime_cfg.instruction_limit = sandbox_policy.instruction_limit;
    runtime_cfg.memory_limit_bytes = sandbox_policy.memory_limit_bytes;
    runtime_cfg.allowed_libraries = sandbox_policy.allowed_libraries;

    server::core::scripting::LuaHookContext hook_ctx{};
    hook_ctx.session_id = 7;
    hook_ctx.user = "installed-consumer";

    server::core::scripting::LuaRuntime::HostValue host_value{std::int64_t{7}};
    server::core::scripting::LuaRuntime runtime(runtime_cfg);
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts{{script_path, "policy"}};
    const auto reload = runtime.reload_scripts(scripts);
    if (!require_true(reload.loaded == 1 && reload.failed == 0, "installed consumer lua reload should succeed")) {
        return 1;
    }
    const auto before = runtime.call_all("on_login", hook_ctx);
    if (!require_true(before.decision == server::core::scripting::LuaHookDecision::kDeny, "installed consumer lua call should honor initial deny")) {
        return 1;
    }
    if (!require_true(before.reason == "installed deny", "installed consumer lua deny reason mismatch")) {
        return 1;
    }

    (void)watcher.poll([](const server::core::scripting::ScriptWatcher::ChangeEvent&) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write_text(script_path, "return { hook = \"on_login\", decision = \"pass\" }\n");

    bool reload_called = false;
    const bool poll_ok = watcher.poll([&](const server::core::scripting::ScriptWatcher::ChangeEvent& event) {
        if (event.kind == server::core::scripting::ScriptWatcher::ChangeKind::kModified
            && event.path.filename() == "policy.lua") {
            reload_called = true;
            (void)runtime.reload_scripts(scripts);
        }
    });
    if (!require_true(poll_ok && reload_called, "installed consumer watcher should reload modified script")) {
        return 1;
    }

    const auto after = runtime.call_all("on_login", hook_ctx);
    if (!require_true(after.decision == server::core::scripting::LuaHookDecision::kPass, "installed consumer lua call should observe reloaded pass")) {
        return 1;
    }

    if (!require_true(server::core::scripting::sandbox::is_library_allowed("string", sandbox_policy), "installed consumer sandbox should allow string")) {
        return 1;
    }
    if (!require_true(server::core::scripting::sandbox::is_symbol_forbidden("require", sandbox_policy), "installed consumer sandbox should forbid require")) {
        return 1;
    }

    const auto runtime_metrics = runtime.metrics_snapshot();
    (void)host_value.is_nil();
    (void)runtime_metrics.loaded_scripts;
    (void)watcher_cfg.recursive;

    std::error_code cleanup_ec;
    std::filesystem::remove_all(temp_dir, cleanup_ec);

    return 0;
}
