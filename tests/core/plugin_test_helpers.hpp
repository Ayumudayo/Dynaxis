#pragma once

#include "plugin_test_api.hpp"

#include "server/core/plugin/plugin_chain_host.hpp"
#include "server/core/plugin/plugin_host.hpp"
#include "server/core/util/paths.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tests::plugin {

using Host = server::core::plugin::PluginHost<TestPluginApi>;

inline std::filesystem::path resolve_module_path(const char* file_name) {
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

inline std::filesystem::path make_temp_dir(const std::string& tag) {
    const auto base = std::filesystem::temp_directory_path();
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = base / ("" + tag + "_" + std::to_string(nonce));
    std::error_code ec;
    (void)std::filesystem::create_directories(dir, ec);
    return dir;
}

inline bool copy_with_mtime_tick(const std::filesystem::path& src,
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
    const auto tick = std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::seconds(2));
    const auto bumped_mtime = previous_mtime.has_value()
        ? std::max(now, *previous_mtime + tick)
        : now;

    ec.clear();
    std::filesystem::last_write_time(dst, bumped_mtime, ec);
    if (ec) {
        error = std::string("last_write_time failed: ") + ec.message();
        return false;
    }

    error.clear();
    return true;
}

inline Host::Config make_host_config(const std::filesystem::path& plugin_path,
                                     const std::filesystem::path& cache_dir,
                                     const std::optional<std::filesystem::path>& lock_path = std::nullopt) {
    Host::Config cfg{};
    cfg.plugin_path = plugin_path;
    cfg.cache_dir = cache_dir;
    cfg.lock_path = lock_path;
    cfg.entrypoint_symbol = kEntrypointSymbol;

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
        if (api->abi_version != kExpectedAbiVersion) {
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

inline server::core::plugin::PluginChainHost<TestPluginApi>::Config make_chain_config(
    const std::filesystem::path& cache_dir,
    const std::optional<std::filesystem::path>& plugins_dir = std::nullopt,
    const std::vector<std::filesystem::path>& plugin_paths = {}) {
    using Chain = server::core::plugin::PluginChainHost<TestPluginApi>;

    Chain::Config cfg{};
    cfg.cache_dir = cache_dir;
    cfg.plugins_dir = plugins_dir;
    cfg.plugin_paths = plugin_paths;

    const auto host_cfg = make_host_config({}, cache_dir);
    cfg.entrypoint_symbol = host_cfg.entrypoint_symbol;
    cfg.api_resolver = host_cfg.api_resolver;
    cfg.api_validator = host_cfg.api_validator;
    cfg.instance_creator = host_cfg.instance_creator;
    cfg.instance_destroyer = host_cfg.instance_destroyer;

    return cfg;
}

} // namespace tests::plugin
