#include <cstdint>
#include <filesystem>
#include <string>
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
    server::core::plugin::PluginChainHost<InstalledConsumerPluginApi>::MetricsSnapshot chain_metrics{};
    (void)chain_metrics.configured;

    server::core::scripting::ScriptWatcher::Config watcher_cfg{};
    watcher_cfg.scripts_dir = std::filesystem::temp_directory_path() / "server_core_installed_consumer_scripts";
    watcher_cfg.extensions = {".lua"};

    server::core::scripting::sandbox::Policy sandbox_policy{};
    sandbox_policy.allowed_libraries = {"base", "string"};
    sandbox_policy.forbidden_symbols = {"os.execute"};
    sandbox_policy.instruction_limit = 10'000;
    sandbox_policy.memory_limit_bytes = 256 * 1024;

    server::core::scripting::LuaRuntime::Config runtime_cfg{};
    runtime_cfg.instruction_limit = sandbox_policy.instruction_limit;
    runtime_cfg.memory_limit_bytes = sandbox_policy.memory_limit_bytes;
    runtime_cfg.allowed_libraries = sandbox_policy.allowed_libraries;

    server::core::scripting::LuaHookContext hook_ctx{};
    hook_ctx.session_id = 7;
    hook_ctx.user = "installed-consumer";

    server::core::scripting::LuaRuntime::HostValue host_value{std::int64_t{7}};
    server::core::scripting::LuaRuntime::MetricsSnapshot runtime_metrics{};
    (void)host_value.is_nil();
    (void)runtime_metrics.loaded_scripts;
    (void)watcher_cfg.recursive;

    return 0;
}
