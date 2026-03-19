#include "server/core/app/engine_runtime.hpp"

#include <thread>

namespace server::core::app {

EngineRuntime::EngineRuntime(std::string name)
    : name_(std::move(name))
    , context_(std::make_unique<EngineContext>())
    , host_(std::make_unique<AppHost>(name_))
    , bridge_state_(std::make_shared<BridgeState>()) {
}

EngineRuntime::~EngineRuntime() = default;

EngineRuntime::EngineRuntime(EngineRuntime&&) noexcept = default;
EngineRuntime& EngineRuntime::operator=(EngineRuntime&&) noexcept = default;

const std::string& EngineRuntime::name() const noexcept {
    return name_;
}

EngineContext& EngineRuntime::context() noexcept {
    return *context_;
}

const EngineContext& EngineRuntime::context() const noexcept {
    return *context_;
}

AppHost& EngineRuntime::host() noexcept {
    return *host_;
}

const AppHost& EngineRuntime::host() const noexcept {
    return *host_;
}

void EngineRuntime::declare_dependency(std::string name, DependencyRequirement requirement) {
    host_->declare_dependency(std::move(name), requirement);
}

void EngineRuntime::set_dependency_ok(std::string_view name, bool ok) {
    host_->set_dependency_ok(name, ok);
}

void EngineRuntime::set_lifecycle_phase(LifecyclePhase phase) noexcept {
    host_->set_lifecycle_phase(phase);
}

EngineRuntime::LifecyclePhase EngineRuntime::lifecycle_phase() const noexcept {
    return host_->lifecycle_phase();
}

void EngineRuntime::set_healthy(bool healthy) noexcept {
    host_->set_healthy(healthy);
}

bool EngineRuntime::healthy() const noexcept {
    return host_->healthy();
}

void EngineRuntime::set_ready(bool ready) noexcept {
    host_->set_ready(ready);
}

bool EngineRuntime::ready() const noexcept {
    return host_->ready();
}

bool EngineRuntime::dependencies_ok() const noexcept {
    return host_->dependencies_ok();
}

void EngineRuntime::mark_running() noexcept {
    set_ready(true);
    set_lifecycle_phase(LifecyclePhase::kRunning);
}

void EngineRuntime::mark_stopped() noexcept {
    set_ready(false);
    set_lifecycle_phase(LifecyclePhase::kStopped);
}

void EngineRuntime::mark_failed() noexcept {
    set_ready(false);
    set_lifecycle_phase(LifecyclePhase::kFailed);
}

bool EngineRuntime::request_stop() noexcept {
    return host_->request_stop();
}

bool EngineRuntime::stop_requested() const noexcept {
    return host_->stop_requested();
}

void EngineRuntime::wait_for_stop(std::chrono::milliseconds poll_interval) const {
    if (poll_interval.count() <= 0) {
        poll_interval = std::chrono::milliseconds(1);
    }

    while (!stop_requested()) {
        std::this_thread::sleep_for(poll_interval);
    }
}

void EngineRuntime::run_shutdown() noexcept {
    request_stop();
    set_ready(false);
    host_->run_shutdown_steps();
}

void EngineRuntime::start_admin_http(unsigned short port,
                                     MetricsCallback metrics_callback,
                                     LogsCallback logs_callback,
                                     RouteCallback route_callback) {
    host_->start_admin_http(
        port,
        std::move(metrics_callback),
        std::move(logs_callback),
        std::move(route_callback));
}

void EngineRuntime::stop_admin_http() {
    host_->stop_admin_http();
}

void EngineRuntime::add_shutdown_step(std::string name, std::function<void()> step) {
    host_->add_shutdown_step(std::move(name), std::move(step));
}

void EngineRuntime::install_process_signal_handlers() {
    server::core::app::install_termination_signal_handlers();
}

void EngineRuntime::install_asio_termination_signals(boost::asio::io_context& io,
                                                     std::function<void()> on_shutdown) {
    host_->install_asio_termination_signals(io, std::move(on_shutdown));
}

EngineRuntime::Snapshot EngineRuntime::snapshot() const {
    return Snapshot{
        .name = name_,
        .lifecycle_phase = lifecycle_phase(),
        .healthy = healthy(),
        .ready = ready(),
        .dependencies_ok = dependencies_ok(),
        .stop_requested = stop_requested(),
        .context_service_count = context_->service_count(),
        .compatibility_bridge_count = compatibility_bridge_count(),
    };
}

void EngineRuntime::clear_global_services() noexcept {
    server::core::util::services::Registry::instance().clear_owned(bridge_owner_token());
    clear_bridge_keys();
}

server::core::util::services::Registry::OwnerToken EngineRuntime::bridge_owner_token() const noexcept {
    return reinterpret_cast<server::core::util::services::Registry::OwnerToken>(bridge_state_.get());
}

void EngineRuntime::remember_bridge_key(std::string key) {
    if (!bridge_state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(bridge_state_->mutex);
    bridge_state_->keys.insert(std::move(key));
}

void EngineRuntime::clear_bridge_keys() noexcept {
    if (!bridge_state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(bridge_state_->mutex);
    bridge_state_->keys.clear();
}

std::size_t EngineRuntime::compatibility_bridge_count() const noexcept {
    if (!bridge_state_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(bridge_state_->mutex);
    return bridge_state_->keys.size();
}

} // namespace server::core::app
