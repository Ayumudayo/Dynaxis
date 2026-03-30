#include <atomic>

#include "server_runtime_state.hpp"

#include "server/chat/chat_service.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/util/service_registry.hpp"

namespace server::app {

namespace services = server::core::util::services;

namespace {

struct BootstrapMetricsState {
    std::atomic<std::uint64_t> subscribe_total{0};
    std::atomic<std::uint64_t> self_echo_drop_total{0};
    std::atomic<long long> subscribe_last_lag_ms{0};
    std::atomic<std::uint64_t> admin_command_verify_ok_total{0};
    std::atomic<std::uint64_t> admin_command_verify_fail_total{0};
    std::atomic<std::uint64_t> admin_command_verify_replay_total{0};
    std::atomic<std::uint64_t> admin_command_verify_signature_mismatch_total{0};
    std::atomic<std::uint64_t> admin_command_verify_expired_total{0};
    std::atomic<std::uint64_t> admin_command_verify_future_total{0};
    std::atomic<std::uint64_t> admin_command_verify_missing_field_total{0};
    std::atomic<std::uint64_t> admin_command_verify_invalid_issued_at_total{0};
    std::atomic<std::uint64_t> admin_command_verify_secret_not_configured_total{0};
    std::atomic<std::uint64_t> admin_command_target_mismatch_total{0};
    std::atomic<std::uint64_t> shutdown_drain_completed_total{0};
    std::atomic<std::uint64_t> shutdown_drain_timeout_total{0};
    std::atomic<std::uint64_t> shutdown_drain_forced_close_total{0};
    std::atomic<std::uint64_t> shutdown_drain_remaining_connections{0};
    std::atomic<long long> shutdown_drain_elapsed_ms{0};
    std::atomic<long long> shutdown_drain_timeout_ms{0};
};

BootstrapMetricsState g_bootstrap_metrics_state;

} // namespace

BootstrapMetricsSnapshot bootstrap_metrics_snapshot() {
    const auto& state = g_bootstrap_metrics_state;
    BootstrapMetricsSnapshot snapshot;
    snapshot.subscribe_total = state.subscribe_total.load(std::memory_order_relaxed);
    snapshot.self_echo_drop_total = state.self_echo_drop_total.load(std::memory_order_relaxed);
    snapshot.subscribe_last_lag_ms = state.subscribe_last_lag_ms.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_ok_total =
        state.admin_command_verify_ok_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_fail_total =
        state.admin_command_verify_fail_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_replay_total =
        state.admin_command_verify_replay_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_signature_mismatch_total =
        state.admin_command_verify_signature_mismatch_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_expired_total =
        state.admin_command_verify_expired_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_future_total =
        state.admin_command_verify_future_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_missing_field_total =
        state.admin_command_verify_missing_field_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_invalid_issued_at_total =
        state.admin_command_verify_invalid_issued_at_total.load(std::memory_order_relaxed);
    snapshot.admin_command_verify_secret_not_configured_total =
        state.admin_command_verify_secret_not_configured_total.load(std::memory_order_relaxed);
    snapshot.admin_command_target_mismatch_total =
        state.admin_command_target_mismatch_total.load(std::memory_order_relaxed);
    snapshot.shutdown_drain_completed_total =
        state.shutdown_drain_completed_total.load(std::memory_order_relaxed);
    snapshot.shutdown_drain_timeout_total =
        state.shutdown_drain_timeout_total.load(std::memory_order_relaxed);
    snapshot.shutdown_drain_forced_close_total =
        state.shutdown_drain_forced_close_total.load(std::memory_order_relaxed);
    snapshot.shutdown_drain_remaining_connections =
        state.shutdown_drain_remaining_connections.load(std::memory_order_relaxed);
    snapshot.shutdown_drain_elapsed_ms =
        state.shutdown_drain_elapsed_ms.load(std::memory_order_relaxed);
    snapshot.shutdown_drain_timeout_ms =
        state.shutdown_drain_timeout_ms.load(std::memory_order_relaxed);
    return snapshot;
}

void reset_bootstrap_shutdown_drain_metrics(const std::uint64_t timeout_ms) {
    auto& state = g_bootstrap_metrics_state;
    state.shutdown_drain_completed_total.store(0, std::memory_order_relaxed);
    state.shutdown_drain_timeout_total.store(0, std::memory_order_relaxed);
    state.shutdown_drain_forced_close_total.store(0, std::memory_order_relaxed);
    state.shutdown_drain_remaining_connections.store(0, std::memory_order_relaxed);
    state.shutdown_drain_elapsed_ms.store(0, std::memory_order_relaxed);
    state.shutdown_drain_timeout_ms.store(static_cast<long long>(timeout_ms), std::memory_order_relaxed);
}

void record_bootstrap_subscribe() {
    g_bootstrap_metrics_state.subscribe_total.fetch_add(1, std::memory_order_relaxed);
}

void record_bootstrap_admin_verify_result(
    const server::core::security::admin_command_auth::VerifyResult result) {
    auto& state = g_bootstrap_metrics_state;
    using VerifyResult = server::core::security::admin_command_auth::VerifyResult;

    switch (result) {
    case VerifyResult::kOk:
        state.admin_command_verify_ok_total.fetch_add(1, std::memory_order_relaxed);
        return;
    case VerifyResult::kReplay:
        state.admin_command_verify_replay_total.fetch_add(1, std::memory_order_relaxed);
        break;
    case VerifyResult::kSignatureMismatch:
        state.admin_command_verify_signature_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        break;
    case VerifyResult::kExpired:
        state.admin_command_verify_expired_total.fetch_add(1, std::memory_order_relaxed);
        break;
    case VerifyResult::kFuture:
        state.admin_command_verify_future_total.fetch_add(1, std::memory_order_relaxed);
        break;
    case VerifyResult::kMissingField:
        state.admin_command_verify_missing_field_total.fetch_add(1, std::memory_order_relaxed);
        break;
    case VerifyResult::kInvalidIssuedAt:
        state.admin_command_verify_invalid_issued_at_total.fetch_add(1, std::memory_order_relaxed);
        break;
    case VerifyResult::kSecretNotConfigured:
        state.admin_command_verify_secret_not_configured_total.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    state.admin_command_verify_fail_total.fetch_add(1, std::memory_order_relaxed);
}

void record_bootstrap_admin_target_mismatch() {
    g_bootstrap_metrics_state.admin_command_target_mismatch_total.fetch_add(
        1,
        std::memory_order_relaxed);
}

void set_bootstrap_shutdown_drain_timeout(const std::uint64_t timeout_ms) {
    g_bootstrap_metrics_state.shutdown_drain_timeout_ms.store(
        static_cast<long long>(timeout_ms),
        std::memory_order_relaxed);
}

void update_bootstrap_shutdown_drain_progress(
    const std::uint64_t remaining_connections,
    const long long elapsed_ms) {
    auto& state = g_bootstrap_metrics_state;
    state.shutdown_drain_remaining_connections.store(
        remaining_connections,
        std::memory_order_relaxed);
    state.shutdown_drain_elapsed_ms.store(elapsed_ms, std::memory_order_relaxed);
}

void record_bootstrap_shutdown_drain_completed() {
    g_bootstrap_metrics_state.shutdown_drain_completed_total.fetch_add(
        1,
        std::memory_order_relaxed);
}

void record_bootstrap_shutdown_drain_timeout(const std::uint64_t forced_close_connections) {
    auto& state = g_bootstrap_metrics_state;
    state.shutdown_drain_timeout_total.fetch_add(1, std::memory_order_relaxed);
    state.shutdown_drain_forced_close_total.fetch_add(
        forced_close_connections,
        std::memory_order_relaxed);
}

bool server_health_ok() {
    if (auto host = services::get<server::core::app::AppHost>()) {
        return host->healthy() && !host->stop_requested();
    }
    return true;
}

bool server_ready_ok() {
    if (auto host = services::get<server::core::app::AppHost>()) {
        return host->ready() && host->healthy() && !host->stop_requested();
    }
    return services::get<server::app::chat::ChatService>() != nullptr;
}

std::string server_health_body(bool ok) {
    if (const auto host = services::get<server::core::app::AppHost>()) {
        return host->health_body(ok);
    }
    return ok ? std::string("ok\n") : std::string("unhealthy\n");
}

std::string server_readiness_body(bool ok) {
    if (const auto host = services::get<server::core::app::AppHost>()) {
        return host->readiness_body(ok);
    }
    return ok ? std::string("ready\n") : std::string("not ready\n");
}

std::optional<server::core::app::EngineRuntime::Snapshot> server_runtime_snapshot() {
    if (const auto runtime = services::get<server::core::app::EngineRuntime>()) {
        return runtime->snapshot();
    }
    return std::nullopt;
}

std::vector<server::core::app::EngineRuntime::ModuleSnapshot> server_runtime_module_snapshot() {
    if (const auto runtime = services::get<server::core::app::EngineRuntime>()) {
        return runtime->module_snapshot();
    }
    return {};
}

std::string server_dependency_metrics_text() {
    if (const auto host = services::get<server::core::app::AppHost>()) {
        return host->dependency_metrics_text();
    }
    return {};
}

std::string server_lifecycle_metrics_text() {
    if (const auto host = services::get<server::core::app::AppHost>()) {
        return host->lifecycle_metrics_text();
    }
    return {};
}

std::optional<server::app::chat::ChatHookPluginsMetrics> server_chat_hook_plugin_metrics() {
    if (const auto chat = services::get<server::app::chat::ChatService>()) {
        return chat->chat_hook_plugins_metrics();
    }
    return std::nullopt;
}

std::optional<server::app::chat::ContinuityMetrics> server_chat_continuity_metrics() {
    if (const auto chat = services::get<server::app::chat::ChatService>()) {
        return chat->continuity_metrics();
    }
    return std::nullopt;
}

std::optional<server::app::chat::LuaHooksMetrics> server_chat_lua_hooks_metrics() {
    if (const auto chat = services::get<server::app::chat::ChatService>()) {
        return chat->lua_hooks_metrics();
    }
    return std::nullopt;
}

} // namespace server::app
