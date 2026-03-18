#include "server/core/app/engine_builder.hpp"

namespace server::core::app {

EngineBuilder::EngineBuilder(std::string name)
    : name_(std::move(name)) {
}

EngineBuilder& EngineBuilder::initial_lifecycle_phase(AppHost::LifecyclePhase phase) noexcept {
    initial_phase_ = phase;
    return *this;
}

EngineBuilder& EngineBuilder::initial_ready(bool ready) noexcept {
    initial_ready_ = ready;
    return *this;
}

EngineBuilder& EngineBuilder::initial_healthy(bool healthy) noexcept {
    initial_healthy_ = healthy;
    return *this;
}

EngineBuilder& EngineBuilder::install_process_signal_handlers(bool enabled) noexcept {
    install_process_signal_handlers_ = enabled;
    return *this;
}

EngineBuilder& EngineBuilder::declare_dependency(std::string name,
                                                 AppHost::DependencyRequirement requirement) {
    dependencies_.push_back(DependencySpec{std::move(name), requirement});
    return *this;
}

EngineBuilder& EngineBuilder::admin_http(unsigned short port,
                                         EngineRuntime::MetricsCallback metrics_callback) {
    admin_http_ = AdminHttpSpec{port, std::move(metrics_callback)};
    return *this;
}

EngineRuntime EngineBuilder::build() & {
    return std::move(*this).build();
}

EngineRuntime EngineBuilder::build() && {
    EngineRuntime runtime(std::move(name_));
    runtime.set_lifecycle_phase(initial_phase_);
    runtime.set_healthy(initial_healthy_);
    runtime.set_ready(initial_ready_);

    if (install_process_signal_handlers_) {
        runtime.install_process_signal_handlers();
    }

    for (auto& dependency : dependencies_) {
        runtime.declare_dependency(std::move(dependency.name), dependency.requirement);
    }

    if (admin_http_.has_value() && admin_http_->port != 0) {
        auto metrics_callback = std::move(admin_http_->metrics_callback);
        if (!metrics_callback) {
            metrics_callback = [] { return std::string{}; };
        }
        runtime.start_admin_http(admin_http_->port, std::move(metrics_callback));
    }

    return runtime;
}

} // namespace server::core::app
