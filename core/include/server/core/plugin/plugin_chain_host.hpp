#pragma once

#include "server/core/plugin/plugin_host.hpp"
#include "server/core/util/log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace server::core::plugin {

/**
 * @brief 다중 플러그인 체인을 구성하고 재스캔/리로드를 관리하는 범용 호스트입니다.
 */
template <typename ApiTable>
class PluginChainHost {
public:
    using Host = PluginHost<ApiTable>;
    using HostList = std::vector<std::shared_ptr<Host>>;

    /** @brief 체인 구성값입니다. */
    struct Config {
        std::vector<std::filesystem::path> plugin_paths;
        std::optional<std::filesystem::path> plugins_dir;
        std::optional<std::filesystem::path> fallback_plugins_dir;
        std::filesystem::path cache_dir;
        std::optional<std::filesystem::path> single_lock_path;
        std::string entrypoint_symbol;
        std::vector<std::string> fallback_entrypoint_symbols;
        typename Host::ApiResolver api_resolver;
        typename Host::ApiValidator api_validator;
        typename Host::InstanceCreator instance_creator;
        typename Host::InstanceDestroyer instance_destroyer;
    };

    /** @brief 체인 메트릭 스냅샷입니다. */
    struct MetricsSnapshot {
        bool configured{false};
        std::string mode; // none|dir|paths|single
        std::vector<typename Host::MetricsSnapshot> plugins;
    };

    explicit PluginChainHost(Config cfg)
        : cfg_(std::move(cfg)) {
        if (cfg_.plugins_dir.has_value() && cfg_.plugins_dir->empty()) {
            cfg_.plugins_dir.reset();
        }
        if (cfg_.fallback_plugins_dir.has_value() && cfg_.fallback_plugins_dir->empty()) {
            cfg_.fallback_plugins_dir.reset();
        }
        if (cfg_.single_lock_path.has_value() && cfg_.single_lock_path->empty()) {
            cfg_.single_lock_path.reset();
        }
    }

    void poll_reload() {
        const bool configured = (!cfg_.plugin_paths.empty() || cfg_.plugins_dir.has_value());
        if (!configured) {
            return;
        }

        std::vector<std::filesystem::path> paths;
        if (!get_desired_paths(paths)) {
            auto ordered = ordered_.load(std::memory_order_acquire);
            if (ordered) {
                for (const auto& host : *ordered) {
                    if (host) {
                        host->poll_reload();
                    }
                }
            }
            return;
        }

        if (paths.empty()) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                by_key_.clear();
            }
            ordered_.store(std::make_shared<HostList>(), std::memory_order_release);
            return;
        }

        const bool single_explicit = (!cfg_.plugins_dir.has_value() && paths.size() == 1 && cfg_.single_lock_path.has_value());

        auto ordered = std::make_shared<HostList>();
        ordered->reserve(paths.size());

        std::unordered_set<std::string> keep;
        keep.reserve(paths.size());

        {
            std::lock_guard<std::mutex> lock(mu_);

            for (const auto& p : paths) {
                const auto key = normalize_key(p);
                keep.insert(key);

                auto it = by_key_.find(key);
                if (it == by_key_.end()) {
                    typename Host::Config host_cfg{};
                    host_cfg.plugin_path = p;
                    host_cfg.cache_dir = cfg_.cache_dir;
                    if (single_explicit) {
                        host_cfg.lock_path = *cfg_.single_lock_path;
                    }
                    host_cfg.entrypoint_symbol = cfg_.entrypoint_symbol;
                    host_cfg.fallback_entrypoint_symbols = cfg_.fallback_entrypoint_symbols;
                    host_cfg.api_resolver = cfg_.api_resolver;
                    host_cfg.api_validator = cfg_.api_validator;
                    host_cfg.instance_creator = cfg_.instance_creator;
                    host_cfg.instance_destroyer = cfg_.instance_destroyer;
                    auto host = std::make_shared<Host>(std::move(host_cfg));
                    it = by_key_.emplace(key, std::move(host)).first;
                }

                ordered->push_back(it->second);
            }

            for (auto it = by_key_.begin(); it != by_key_.end();) {
                if (keep.count(it->first) == 0) {
                    it = by_key_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        ordered_.store(ordered, std::memory_order_release);

        for (const auto& host : *ordered) {
            if (host) {
                host->poll_reload();
            }
        }
    }

    std::shared_ptr<const HostList> current_chain() const {
        return ordered_.load(std::memory_order_acquire);
    }

    MetricsSnapshot metrics_snapshot() const {
        MetricsSnapshot out{};
        out.configured = (!cfg_.plugin_paths.empty() || cfg_.plugins_dir.has_value());

        if (!out.configured) {
            out.mode = "none";
            return out;
        }

        if (cfg_.plugins_dir.has_value()) {
            out.mode = "dir";
        } else if (!cfg_.plugin_paths.empty()) {
            if (cfg_.plugin_paths.size() == 1 && cfg_.single_lock_path.has_value()) {
                out.mode = "single";
            } else {
                out.mode = "paths";
            }
        } else {
            out.mode = "none";
        }

        auto ordered = ordered_.load(std::memory_order_acquire);
        if (!ordered) {
            return out;
        }

        out.plugins.reserve(ordered->size());
        for (const auto& host : *ordered) {
            if (!host) {
                continue;
            }
            out.plugins.push_back(host->metrics_snapshot());
        }

        return out;
    }

private:
    static std::string module_extension() {
#if defined(_WIN32)
        return ".dll";
#elif defined(__APPLE__)
        return ".dylib";
#else
        return ".so";
#endif
    }

    static std::string normalize_key(const std::filesystem::path& p) {
        auto s = p.lexically_normal().generic_string();
#if defined(_WIN32)
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
        return s;
    }

    bool scan_plugins_dir(const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) const {
        out.clear();

        const auto ext = module_extension();
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        if (ec) {
            server::core::log::warn(std::string("plugin_chain_host: failed to scan plugins dir: ") + dir.string());
            return false;
        }

        for (const auto& entry : it) {
            std::error_code st_ec;
            if (!entry.is_regular_file(st_ec) || st_ec) {
                continue;
            }
            const auto p = entry.path();
            if (p.extension().string() != ext) {
                continue;
            }
            out.push_back(p);
        }

        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.filename().string() < b.filename().string();
        });
        return true;
    }

    bool get_desired_paths(std::vector<std::filesystem::path>& out) const {
        out.clear();

        if (!cfg_.plugin_paths.empty()) {
            out.reserve(cfg_.plugin_paths.size());
            for (const auto& p : cfg_.plugin_paths) {
                if (!p.empty()) {
                    out.push_back(p);
                }
            }
            return true;
        }

        if (!cfg_.plugins_dir.has_value()) {
            return true;
        }

        std::vector<std::filesystem::path> primary_paths;
        if (!scan_plugins_dir(*cfg_.plugins_dir, primary_paths)) {
            if (!cfg_.fallback_plugins_dir.has_value()) {
                return false;
            }
            return scan_plugins_dir(*cfg_.fallback_plugins_dir, out);
        }

        if (!primary_paths.empty()) {
            out = std::move(primary_paths);
            return true;
        }

        if (cfg_.fallback_plugins_dir.has_value()) {
            return scan_plugins_dir(*cfg_.fallback_plugins_dir, out);
        }

        return true;
    }

    Config cfg_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Host>> by_key_;
    std::atomic<std::shared_ptr<const HostList>> ordered_{};
};

} // namespace server::core::plugin
