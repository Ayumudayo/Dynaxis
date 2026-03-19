#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "plugin_test_api.hpp"

#include "server/core/api/version.hpp"
#include "server/core/plugin/plugin_chain_host.hpp"
#include "server/core/plugin/plugin_host.hpp"
#include "server/core/plugin/shared_library.hpp"
#include "server/core/scripting/lua_runtime.hpp"
#include "server/core/scripting/lua_sandbox.hpp"
#include "server/core/scripting/script_watcher.hpp"
#include "server/core/util/paths.hpp"

#ifndef TEST_PLUGIN_RUNTIME_V1_FILE
#error "TEST_PLUGIN_RUNTIME_V1_FILE is not defined"
#endif

#ifndef TEST_PLUGIN_RUNTIME_V2_FILE
#error "TEST_PLUGIN_RUNTIME_V2_FILE is not defined"
#endif

namespace {

using TestPluginApi = tests::plugin::TestPluginApi;
using GetApiFn = tests::plugin::GetApiFn;
using Host = server::core::plugin::PluginHost<TestPluginApi>;
using Chain = server::core::plugin::PluginChainHost<TestPluginApi>;

bool require_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path resolve_module_path(const char* file_name) {
    const auto exe_dir = server::core::util::paths::executable_dir();
    const auto from_exe = exe_dir / file_name;
    if (std::filesystem::exists(from_exe)) {
        return from_exe;
    }

    const auto from_cwd = std::filesystem::current_path() / file_name;
    if (std::filesystem::exists(from_cwd)) {
        return from_cwd;
    }

    return from_exe;
}

std::filesystem::path make_temp_dir(const std::string& tag) {
    const auto base = std::filesystem::temp_directory_path();
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = base / (tag + "_" + std::to_string(nonce));
    std::error_code ec;
    (void)std::filesystem::create_directories(dir, ec);
    return dir;
}

bool copy_with_mtime_tick(
    const std::filesystem::path& src,
    const std::filesystem::path& dst,
    std::string& error) {
    std::error_code previous_ec;
    const auto previous_mtime = std::filesystem::exists(dst)
        ? std::optional<std::filesystem::file_time_type>(std::filesystem::last_write_time(dst, previous_ec))
        : std::nullopt;

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    std::error_code ec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    const auto tick =
        std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::seconds(2));
    const auto bumped_mtime = previous_mtime.has_value() ? std::max(now, *previous_mtime + tick) : now;

    ec.clear();
    std::filesystem::last_write_time(dst, bumped_mtime, ec);
    if (ec) {
        error = std::string("last_write_time failed: ") + ec.message();
        return false;
    }

    error.clear();
    return true;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
}

Host::Config make_host_config(
    const std::filesystem::path& plugin_path,
    const std::filesystem::path& cache_dir) {
    Host::Config cfg{};
    cfg.plugin_path = plugin_path;
    cfg.cache_dir = cache_dir;
    cfg.entrypoint_symbol = tests::plugin::kEntrypointSymbol;
    cfg.api_resolver = [](void* symbol, std::string& error) -> const TestPluginApi* {
        if (!symbol) {
            error = "null symbol";
            return nullptr;
        }

        auto fn = reinterpret_cast<GetApiFn>(symbol);
        try {
            return fn();
        } catch (...) {
            error = "entrypoint threw";
            return nullptr;
        }
    };
    cfg.api_validator = [](const TestPluginApi* api, std::string& error) -> bool {
        if (!api) {
            error = "api is null";
            return false;
        }
        if (api->abi_version != tests::plugin::kExpectedAbiVersion) {
            error = "unexpected abi version";
            return false;
        }
        if (!api->name || !api->transform) {
            error = "api fields are missing";
            return false;
        }
        return true;
    };
    return cfg;
}

Chain::Config make_chain_config(
    const std::filesystem::path& cache_dir,
    const std::filesystem::path& plugins_dir) {
    Chain::Config cfg{};
    cfg.cache_dir = cache_dir;
    cfg.plugins_dir = plugins_dir;

    const auto host_cfg = make_host_config({}, cache_dir);
    cfg.entrypoint_symbol = host_cfg.entrypoint_symbol;
    cfg.api_resolver = host_cfg.api_resolver;
    cfg.api_validator = host_cfg.api_validator;
    cfg.instance_creator = host_cfg.instance_creator;
    cfg.instance_destroyer = host_cfg.instance_destroyer;
    return cfg;
}

std::vector<std::string> loaded_plugin_names(const std::shared_ptr<const Chain::HostList>& chain) {
    std::vector<std::string> names;
    if (!chain) {
        return names;
    }

    names.reserve(chain->size());
    for (const auto& host : *chain) {
        if (!host) {
            continue;
        }
        const auto loaded = host->current();
        if (!loaded || !loaded->api || !loaded->api->name) {
            continue;
        }
        names.emplace_back(loaded->api->name);
    }

    return names;
}

} // namespace

int main() {
    (void)server::core::api::version_string();

    const auto module_v1_path = resolve_module_path(TEST_PLUGIN_RUNTIME_V1_FILE);
    const auto module_v2_path = resolve_module_path(TEST_PLUGIN_RUNTIME_V2_FILE);

    if (!require_true(std::filesystem::exists(module_v1_path), "plugin v1 module must exist")) {
        return 1;
    }
    if (!require_true(std::filesystem::exists(module_v2_path), "plugin v2 module must exist")) {
        return 1;
    }

    std::string error;
    server::core::plugin::SharedLibrary library;
    if (!require_true(library.open(module_v1_path, error), error.c_str())) {
        return 1;
    }
    void* symbol = library.symbol(tests::plugin::kEntrypointSymbol, error);
    if (!require_true(symbol != nullptr, error.c_str())) {
        return 1;
    }
    auto get_api = reinterpret_cast<GetApiFn>(symbol);
    const auto* api = get_api();
    if (!require_true(api != nullptr, "shared library api must resolve")) {
        return 1;
    }
    if (!require_true(api->transform(10) == 11, "shared library plugin should transform with v1 behavior")) {
        return 1;
    }
    library.close();

    const auto temp_dir = make_temp_dir("public_api_extensibility_smoke");
    const auto cache_dir = temp_dir / "cache";
    const auto plugins_dir = temp_dir / "plugins";
    std::filesystem::create_directories(cache_dir);
    std::filesystem::create_directories(plugins_dir);

    const auto live_plugin_path = temp_dir / ("active" + module_v1_path.extension().string());
    std::filesystem::copy_file(module_v1_path, live_plugin_path, std::filesystem::copy_options::overwrite_existing);

    Host host(make_host_config(live_plugin_path, cache_dir));
    host.poll_reload();
    auto current = host.current();
    if (!require_true(current && current->api, "plugin host must load the first plugin")) {
        return 1;
    }
    if (!require_true(current->api->transform(10) == 11, "plugin host should expose v1 behavior")) {
        return 1;
    }

    if (!require_true(copy_with_mtime_tick(module_v2_path, live_plugin_path, error), error.c_str())) {
        return 1;
    }
    host.poll_reload();
    current = host.current();
    if (!require_true(current && current->api, "plugin host must keep a loaded plugin after reload")) {
        return 1;
    }
    if (!require_true(current->api->transform(10) == 12, "plugin host should swap to v2 behavior")) {
        return 1;
    }

    std::filesystem::copy_file(
        module_v2_path,
        plugins_dir / ("20_second" + module_v2_path.extension().string()),
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(
        module_v1_path,
        plugins_dir / ("10_first" + module_v1_path.extension().string()),
        std::filesystem::copy_options::overwrite_existing);

    Chain chain(make_chain_config(temp_dir / "chain_cache", plugins_dir));
    chain.poll_reload();
    const auto names = loaded_plugin_names(chain.current_chain());
    if (!require_true(names.size() == 2, "plugin chain must load two plugins")) {
        return 1;
    }
    if (!require_true(names[0] == "plugin_v1" && names[1] == "plugin_v2", "plugin chain must follow filename order")) {
        return 1;
    }

    auto sandbox_policy = server::core::scripting::sandbox::default_policy();
    if (!require_true(
            server::core::scripting::sandbox::is_library_allowed("string", sandbox_policy),
            "lua sandbox must allow string library")) {
        return 1;
    }
    if (!require_true(
            server::core::scripting::sandbox::is_symbol_forbidden("require", sandbox_policy),
            "lua sandbox must forbid require")) {
        return 1;
    }

    const auto scripts_dir = temp_dir / "scripts";
    std::filesystem::create_directories(scripts_dir);
    const auto script_path = scripts_dir / "policy.lua";
    write_text(script_path, "return { hook = \"on_login\", decision = \"pass\" }\n");

    server::core::scripting::ScriptWatcher::Config watcher_cfg{};
    watcher_cfg.scripts_dir = scripts_dir;
    watcher_cfg.extensions = {".lua"};
    server::core::scripting::ScriptWatcher watcher(watcher_cfg);

    server::core::scripting::LuaRuntime runtime;
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back({script_path, "policy"});
    const auto initial_reload = runtime.reload_scripts(scripts);
    if (!require_true(initial_reload.loaded == 1 && initial_reload.failed == 0, "lua runtime must reload one script")) {
        return 1;
    }

    const auto baseline = runtime.call_all("on_login");
    if (!require_true(baseline.decision == server::core::scripting::LuaHookDecision::kPass, "baseline lua decision must pass")) {
        return 1;
    }

    (void)watcher.poll([](const server::core::scripting::ScriptWatcher::ChangeEvent&) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write_text(script_path, "return { hook = \"on_login\", decision = \"deny\", reason = \"reloaded deny\" }\n");

    bool reload_called = false;
    bool modified_seen = false;
    const bool poll_ok = watcher.poll([&](const server::core::scripting::ScriptWatcher::ChangeEvent& event) {
        if (event.path.filename() == "policy.lua"
            && event.kind == server::core::scripting::ScriptWatcher::ChangeKind::kModified) {
            modified_seen = true;
            reload_called = true;
            (void)runtime.reload_scripts(scripts);
        }
    });
    if (!require_true(poll_ok, "script watcher poll should succeed")) {
        return 1;
    }
    if (!require_true(modified_seen && reload_called, "script watcher must detect modified lua script")) {
        return 1;
    }

    const auto after_reload = runtime.call_all("on_login");
    if (!require_true(after_reload.decision == server::core::scripting::LuaHookDecision::kDeny, "lua runtime must observe reloaded deny directive")) {
        return 1;
    }
    if (!require_true(after_reload.reason == "reloaded deny", "lua runtime must preserve deny reason")) {
        return 1;
    }

    std::error_code cleanup_ec;
    std::filesystem::remove_all(temp_dir, cleanup_ec);
    return 0;
}
