#pragma once

#include <optional>
#include <string>
#include <vector>

#include "server/core/app/engine_runtime.hpp"

namespace server::core::app {

/**
 * @brief 공통 bootstrap 규약으로 `EngineRuntime`를 구성하는 빌더입니다.
 *
 * 목표는 앱별 세부 로직을 숨기는 것이 아니라,
 * lifecycle/dependency/admin-http/signal 초기값을 같은 방식으로 선언하도록 강제하는 것입니다.
 */
class EngineBuilder {
public:
    /** @brief Runtime 조립 전에 선언하는 dependency 요구사항 한 건입니다. */
    struct DependencySpec {
        std::string name;
        AppHost::DependencyRequirement requirement{AppHost::DependencyRequirement::kRequired};
    };

    explicit EngineBuilder(std::string name);

    EngineBuilder& initial_lifecycle_phase(AppHost::LifecyclePhase phase) noexcept;
    EngineBuilder& initial_ready(bool ready) noexcept;
    EngineBuilder& initial_healthy(bool healthy) noexcept;
    EngineBuilder& install_process_signal_handlers(bool enabled = true) noexcept;
    EngineBuilder& declare_dependency(std::string name,
                                      AppHost::DependencyRequirement requirement = AppHost::DependencyRequirement::kRequired);
    EngineBuilder& admin_http(unsigned short port, EngineRuntime::MetricsCallback metrics_callback);

    [[nodiscard]] EngineRuntime build() &;
    [[nodiscard]] EngineRuntime build() &&;

private:
    std::string name_;
    AppHost::LifecyclePhase initial_phase_{AppHost::LifecyclePhase::kBootstrapping};
    bool initial_ready_{false};
    bool initial_healthy_{true};
    bool install_process_signal_handlers_{false};
    std::vector<DependencySpec> dependencies_;

    /** @brief 선택적인 admin HTTP 노출 설정입니다. */
    struct AdminHttpSpec {
        unsigned short port{0};
        EngineRuntime::MetricsCallback metrics_callback;
    };
    std::optional<AdminHttpSpec> admin_http_;
};

} // namespace server::core::app
