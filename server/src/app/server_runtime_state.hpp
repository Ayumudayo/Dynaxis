#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "server/chat/chat_metrics.hpp"
#include "server/core/app/engine_runtime.hpp"

namespace server::app {

struct BootstrapMetricsSnapshot {
    std::uint64_t subscribe_total{0};
    std::uint64_t self_echo_drop_total{0};
    long long subscribe_last_lag_ms{0};
    std::uint64_t shutdown_drain_completed_total{0};
    std::uint64_t shutdown_drain_timeout_total{0};
    std::uint64_t shutdown_drain_forced_close_total{0};
    std::uint64_t shutdown_drain_remaining_connections{0};
    long long shutdown_drain_elapsed_ms{0};
    long long shutdown_drain_timeout_ms{0};
};

BootstrapMetricsSnapshot bootstrap_metrics_snapshot();
void reset_bootstrap_shutdown_drain_metrics(std::uint64_t timeout_ms);
void record_bootstrap_subscribe();

void set_bootstrap_shutdown_drain_timeout(std::uint64_t timeout_ms);
void update_bootstrap_shutdown_drain_progress(
    std::uint64_t remaining_connections,
    long long elapsed_ms);
void record_bootstrap_shutdown_drain_completed();
void record_bootstrap_shutdown_drain_timeout(std::uint64_t forced_close_connections);

bool server_health_ok();
bool server_ready_ok();
std::string server_health_body(bool ok);
std::string server_readiness_body(bool ok);

std::optional<server::core::app::EngineRuntime::Snapshot> server_runtime_snapshot();
std::vector<server::core::app::EngineRuntime::ModuleSnapshot> server_runtime_module_snapshot();
std::string server_dependency_metrics_text();
std::string server_lifecycle_metrics_text();
std::optional<server::app::chat::ContinuityMetrics> server_chat_continuity_metrics();

} // namespace server::app
