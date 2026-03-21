#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include <boost/asio/io_context.hpp>

#include "server/core/app/app_host.hpp"
#include "server/core/app/engine_context.hpp"

namespace server::core::app {

/**
 * @brief `AppHost`와 instance-scoped `EngineContext`를 함께 소유하는 공용 런타임 래퍼입니다.
 *
 * 이 타입의 목적은 각 앱 bootstrap이 lifecycle, dependency, shutdown, context 조립을
 * 같은 방식으로 수행하게 만드는 데 있습니다. 반대로 listeners, routes, workers 같은
 * 앱별 의미는 바깥에 남겨 두어, 공용 runtime 규약과 제품 조합 로직을 분리합니다.
 */
class EngineRuntime {
public:
    using LifecyclePhase = AppHost::LifecyclePhase;
    using DependencyRequirement = AppHost::DependencyRequirement;
    using MetricsCallback = server::core::metrics::MetricsHttpServer::MetricsCallback;
    using LogsCallback = server::core::metrics::MetricsHttpServer::LogsCallback;
    using RouteCallback = server::core::metrics::MetricsHttpServer::RouteCallback;

    /** @brief canonical consumer가 읽는 instance-scoped lifecycle/service ownership 스냅샷입니다. */
    struct Snapshot {
        std::string name;
        LifecyclePhase lifecycle_phase{LifecyclePhase::kInit};
        bool healthy{true};
        bool ready{false};
        bool dependencies_ok{true};
        bool stop_requested{false};
        std::size_t context_service_count{0};
        std::size_t compatibility_bridge_count{0};
    };

    explicit EngineRuntime(std::string name);
    ~EngineRuntime();

    EngineRuntime(EngineRuntime&&) noexcept;
    EngineRuntime& operator=(EngineRuntime&&) noexcept;

    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;

    const std::string& name() const noexcept;

    EngineContext& context() noexcept;
    const EngineContext& context() const noexcept;

    AppHost& host() noexcept;
    const AppHost& host() const noexcept;

    template <typename T>
    void set_service(std::shared_ptr<T> service) {
        context().set(std::move(service));
    }

    template <typename T>
    void set_alias(T& instance) {
        set_service(alias(instance));
    }

    template <typename T>
    void bridge_service(std::shared_ptr<T> service) {
        context().set(service);
        const auto key = server::core::util::services::detail::type_key<T>();
        server::core::util::services::Registry::instance().set_owned<T>(bridge_owner_token(), std::move(service));
        remember_bridge_key(key);
    }

    template <typename T>
    void bridge_alias(T& instance) {
        bridge_service(alias(instance));
    }

    template <typename T>
    static std::shared_ptr<T> alias(T& instance) {
        return std::shared_ptr<T>(&instance, [](T*) {});
    }

    void declare_dependency(std::string name,
                            DependencyRequirement requirement = DependencyRequirement::kRequired);
    void set_dependency_ok(std::string_view name, bool ok);

    void set_lifecycle_phase(LifecyclePhase phase) noexcept;
    LifecyclePhase lifecycle_phase() const noexcept;

    void set_healthy(bool healthy) noexcept;
    bool healthy() const noexcept;

    void set_ready(bool ready) noexcept;
    bool ready() const noexcept;
    bool dependencies_ok() const noexcept;
    void mark_running() noexcept;
    void mark_stopped() noexcept;
    void mark_failed() noexcept;

    bool request_stop() noexcept;
    bool stop_requested() const noexcept;
    void wait_for_stop(std::chrono::milliseconds poll_interval = std::chrono::milliseconds(100)) const;
    void run_shutdown() noexcept;

    void start_admin_http(unsigned short port,
                          MetricsCallback metrics_callback,
                          LogsCallback logs_callback = {},
                          RouteCallback route_callback = {});
    void stop_admin_http();

    void add_shutdown_step(std::string name, std::function<void()> step);

    void install_process_signal_handlers();
    void install_asio_termination_signals(boost::asio::io_context& io,
                                          std::function<void()> on_shutdown = {});
    [[nodiscard]] Snapshot snapshot() const;
    void clear_global_services() noexcept;

private:
    /** @brief runtime이 올린 compatibility bridge key 집합을 추적하는 공유 상태입니다. */
    struct BridgeState {
        mutable std::mutex mutex;
        std::unordered_set<std::string> keys;
    };

    [[nodiscard]] server::core::util::services::Registry::OwnerToken bridge_owner_token() const noexcept;
    void remember_bridge_key(std::string key);
    void clear_bridge_keys() noexcept;
    [[nodiscard]] std::size_t compatibility_bridge_count() const noexcept;

    std::string name_;
    std::unique_ptr<EngineContext> context_;
    std::unique_ptr<AppHost> host_;
    std::shared_ptr<BridgeState> bridge_state_;
};

} // namespace server::core::app
