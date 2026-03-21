#pragma once

#include "server/core/plugin/shared_library.hpp"
#include "server/core/util/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace server::core::plugin {

namespace detail {

inline std::optional<std::filesystem::file_time_type> get_mtime(const std::filesystem::path& p) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return std::nullopt;
    }
    return t;
}

inline bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    const bool ok = std::filesystem::exists(p, ec);
    if (ec) {
        return false;
    }
    return ok;
}

inline bool ensure_dir(const std::filesystem::path& p) {
    std::error_code ec;
    (void)std::filesystem::create_directories(p, ec);
    return !ec;
}

inline std::filesystem::path make_default_lock_path(const std::filesystem::path& plugin_path) {
    const auto dir = plugin_path.parent_path();
    const auto stem = plugin_path.stem().string();
    return dir / (stem + "_LOCK");
}

inline std::filesystem::path make_cache_path(const std::filesystem::path& cache_dir,
                                             const std::filesystem::path& plugin_path,
                                             std::uint64_t seq) {
    const auto stem = plugin_path.stem().string();
    const auto ext = plugin_path.extension().string();
    return cache_dir / (stem + "_" + std::to_string(seq) + ext);
}

inline bool copy_to_cache(const std::filesystem::path& src,
                          const std::filesystem::path& dst,
                          std::string& error) {
    error.clear();

    std::error_code ec;
    auto tmp = dst;
    tmp += ".tmp";

    (void)std::filesystem::remove(tmp, ec);
    ec.clear();

    std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = std::string("copy_file failed: ") + ec.message();
        return false;
    }

    ec.clear();
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::error_code copy_ec;
        std::filesystem::copy_file(tmp, dst, std::filesystem::copy_options::overwrite_existing, copy_ec);
        std::error_code rm_ec;
        (void)std::filesystem::remove(tmp, rm_ec);
        if (copy_ec) {
            error = std::string("rename failed: ") + ec.message() + "; copy fallback failed: " + copy_ec.message();
            return false;
        }
    }

    return true;
}

} // namespace detail

/**
 * @brief 단일 플러그인의 로드/리로드를 관리하는 범용 호스트입니다.
 *
 * `ApiTable`은 플러그인 ABI 함수 테이블 타입입니다. 이 호스트는 cache-copy,
 * lock/sentinel, mtime polling 같은 "안전한 교체 메커니즘"을 공용으로 제공하고,
 * ABI 해석/검증/인스턴스 생성 같은 domain-specific 의미는 콜백으로 주입받습니다.
 *
 * 이렇게 분리하는 이유는, reload 메커니즘 자체는 재사용 가능하지만 실제 ABI 의미는
 * 앱마다 다를 수 있기 때문입니다.
 */
template <typename ApiTable>
class PluginHost {
public:
    using ApiResolver = std::function<const ApiTable*(void* symbol, std::string& error)>;
    using ApiValidator = std::function<bool(const ApiTable* api, std::string& error)>;
    using InstanceCreator = std::function<void*(const ApiTable* api, std::string& error)>;
    using InstanceDestroyer = std::function<void(const ApiTable* api, void* instance)>;

    /** @brief 호스트 구성값입니다. 파일 감시와 ABI 해석 정책을 함께 묶습니다. */
    struct Config {
        std::filesystem::path plugin_path;
        std::filesystem::path cache_dir;
        std::optional<std::filesystem::path> lock_path;
        std::string entrypoint_symbol;
        std::vector<std::string> fallback_entrypoint_symbols;
        ApiResolver api_resolver;
        ApiValidator api_validator;
        InstanceCreator instance_creator;
        InstanceDestroyer instance_destroyer;
    };

    /** @brief 현재 로드된 플러그인 핸들입니다. cache-copy된 라이브러리와 생성된 인스턴스를 함께 소유합니다. */
    struct LoadedPlugin {
        SharedLibrary lib;
        const ApiTable* api{nullptr};
        void* instance{nullptr};
        std::filesystem::path cached_path;
        InstanceDestroyer destroy_instance;

        ~LoadedPlugin() {
            if (api && destroy_instance) {
                try {
                    destroy_instance(api, instance);
                } catch (...) {
                }
            }

            lib.close();
            std::error_code ec;
            (void)std::filesystem::remove(cached_path, ec);
        }
    };

    /** @brief 로드/리로드 상태 메트릭 스냅샷입니다. 운영에서 reload가 실제로 일어나고 있는지 확인할 때 사용합니다. */
    struct MetricsSnapshot {
        std::filesystem::path plugin_path;
        bool loaded{false};
        std::uint64_t reload_attempt_total{0};
        std::uint64_t reload_success_total{0};
        std::uint64_t reload_failure_total{0};
    };

    explicit PluginHost(Config cfg)
        : cfg_(std::move(cfg)) {
        if (cfg_.lock_path.has_value() && cfg_.lock_path->empty()) {
            cfg_.lock_path.reset();
        }
        if (cfg_.cache_dir.empty()) {
            std::error_code ec;
            cfg_.cache_dir = std::filesystem::temp_directory_path(ec) / "plugin_cache";
        }
        if (!cfg_.lock_path.has_value() && !cfg_.plugin_path.empty()) {
            cfg_.lock_path = detail::make_default_lock_path(cfg_.plugin_path);
        }
        if (cfg_.entrypoint_symbol.empty()) {
            cfg_.entrypoint_symbol = "plugin_api_v1";
        }

        cfg_.fallback_entrypoint_symbols.erase(
            std::remove_if(cfg_.fallback_entrypoint_symbols.begin(),
                           cfg_.fallback_entrypoint_symbols.end(),
                           [this](const std::string& symbol) {
                               return symbol.empty() || symbol == cfg_.entrypoint_symbol;
                           }),
            cfg_.fallback_entrypoint_symbols.end());
    }

    /**
     * @brief 플러그인 파일 상태를 점검하고 필요하면 안전하게 reload를 시도합니다.
     *
     * 이 함수는 "mtime이 바뀌었다고 무조건 바로 교체"하지 않습니다. lock/sentinel,
     * cache-copy, entrypoint 검증, API validator, instance 생성이 모두 통과해야만 현재
     * 플러그인을 교체합니다. 그래야 반쯤 배포된 artifact를 읽는 실수를 줄일 수 있습니다.
     */
    void poll_reload() {
        std::lock_guard<std::mutex> lock(reload_mu_);
        if (cfg_.plugin_path.empty()) {
            return;
        }
        if (!detail::file_exists(cfg_.plugin_path)) {
            return;
        }
        if (cfg_.lock_path.has_value() && detail::file_exists(*cfg_.lock_path)) {
            return;
        }

        const auto mtime = detail::get_mtime(cfg_.plugin_path);
        if (!mtime.has_value()) {
            return;
        }
        if (last_attempt_mtime_.has_value() && *last_attempt_mtime_ == *mtime) {
            return;
        }

        reload_attempt_total_.fetch_add(1, std::memory_order_relaxed);
        last_attempt_mtime_ = *mtime;

        const auto record_failure = [this]() {
            reload_failure_total_.fetch_add(1, std::memory_order_relaxed);
        };

        if (!detail::ensure_dir(cfg_.cache_dir)) {
            server::core::log::warn("plugin_host: failed to create cache dir: " + cfg_.cache_dir.string());
            record_failure();
            return;
        }

        const auto seq = ++g_cache_seq_;
        const auto cached = detail::make_cache_path(cfg_.cache_dir, cfg_.plugin_path, seq);

        std::string copy_err;
        if (!detail::copy_to_cache(cfg_.plugin_path, cached, copy_err)) {
            server::core::log::warn("plugin_host: cache copy failed: " + copy_err);
            record_failure();
            return;
        }

        auto mod = std::make_shared<LoadedPlugin>();
        mod->cached_path = cached;

        std::string open_err;
        if (!mod->lib.open(cached, open_err)) {
            server::core::log::warn("plugin_host: library open failed: " + open_err);
            record_failure();
            return;
        }

        std::string sym_err;
        void* sym = nullptr;
        std::string attempted_symbols;
        const auto entrypoint_candidates = build_entrypoint_candidates();
        for (const auto& entrypoint : entrypoint_candidates) {
            std::string current_err;
            sym = mod->lib.symbol(entrypoint.c_str(), current_err);
            if (sym) {
                break;
            }

            if (!attempted_symbols.empty()) {
                attempted_symbols += ", ";
            }
            attempted_symbols += entrypoint;
            if (sym_err.empty() && !current_err.empty()) {
                sym_err = current_err;
            }
        }

        if (!sym) {
            server::core::log::warn("plugin_host: missing entrypoint [" + attempted_symbols + "]: " + sym_err);
            record_failure();
            return;
        }

        std::string api_err;
        const ApiTable* api = nullptr;
        if (cfg_.api_resolver) {
            api = cfg_.api_resolver(sym, api_err);
        } else {
            using GetApiFn = const ApiTable* (*)();
            auto get_api = reinterpret_cast<GetApiFn>(sym);
            try {
                api = get_api();
            } catch (...) {
                api = nullptr;
                api_err = "entrypoint threw exception";
            }
        }
        if (!api) {
            server::core::log::warn("plugin_host: api resolve failed: " + (api_err.empty() ? std::string("null api") : api_err));
            record_failure();
            return;
        }

        std::string validate_err;
        bool valid = true;
        if (cfg_.api_validator) {
            valid = cfg_.api_validator(api, validate_err);
        }
        if (!valid) {
            server::core::log::warn("plugin_host: api validation failed: " + validate_err);
            record_failure();
            return;
        }

        std::string create_err;
        void* instance = nullptr;
        if (cfg_.instance_creator) {
            instance = cfg_.instance_creator(api, create_err);
            if (!create_err.empty()) {
                server::core::log::warn("plugin_host: instance creation failed: " + create_err);
                record_failure();
                return;
            }
        }

        mod->api = api;
        mod->instance = instance;
        mod->destroy_instance = cfg_.instance_destroyer;

        current_.store(std::move(mod), std::memory_order_release);
        reload_success_total_.fetch_add(1, std::memory_order_relaxed);
    }

    /** @brief 현재 성공적으로 로드된 플러그인 핸들을 반환합니다. */
    std::shared_ptr<const LoadedPlugin> current() const {
        return current_.load(std::memory_order_acquire);
    }

    /** @brief 현재 로드 상태와 reload 카운터를 스냅샷으로 반환합니다. */
    MetricsSnapshot metrics_snapshot() const {
        MetricsSnapshot snap{};
        snap.plugin_path = cfg_.plugin_path;
        snap.reload_attempt_total = reload_attempt_total_.load(std::memory_order_relaxed);
        snap.reload_success_total = reload_success_total_.load(std::memory_order_relaxed);
        snap.reload_failure_total = reload_failure_total_.load(std::memory_order_relaxed);
        snap.loaded = static_cast<bool>(current_.load(std::memory_order_acquire));
        return snap;
    }

private:
    std::vector<std::string> build_entrypoint_candidates() const {
        std::vector<std::string> out;
        out.reserve(1 + cfg_.fallback_entrypoint_symbols.size());

        if (!cfg_.entrypoint_symbol.empty()) {
            out.push_back(cfg_.entrypoint_symbol);
        }

        for (const auto& candidate : cfg_.fallback_entrypoint_symbols) {
            if (candidate.empty()) {
                continue;
            }
            const bool duplicate = std::find(out.begin(), out.end(), candidate) != out.end();
            if (!duplicate) {
                out.push_back(candidate);
            }
        }

        if (out.empty()) {
            out.push_back("plugin_api_v1");
        }

        return out;
    }

    Config cfg_;
    std::atomic<std::shared_ptr<LoadedPlugin>> current_{};
    std::optional<std::filesystem::file_time_type> last_attempt_mtime_{};
    mutable std::mutex reload_mu_;

    std::atomic<std::uint64_t> reload_attempt_total_{0};
    std::atomic<std::uint64_t> reload_success_total_{0};
    std::atomic<std::uint64_t> reload_failure_total_{0};

    inline static std::atomic<std::uint64_t> g_cache_seq_{0};
};

} // namespace server::core::plugin
