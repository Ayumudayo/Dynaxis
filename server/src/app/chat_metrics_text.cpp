#include "chat_metrics_text.hpp"

#include <array>
#include <iomanip>
#include <ostream>

namespace server::app {

namespace {

constexpr std::array<std::uint64_t, 12> kPluginHookDurationBucketUpperBoundsNs{{
    1'000,
    2'000,
    5'000,
    10'000,
    25'000,
    50'000,
    100'000,
    250'000,
    500'000,
    1'000'000,
    5'000'000,
    10'000'000,
}};

} // namespace

void MetricsTextWriter::append_counter(const char* name, std::uint64_t value) const {
    stream << "# TYPE " << name << " counter\n" << name << ' ' << value << '\n';
}

void MetricsTextWriter::append_gauge(const char* name, long double value) const {
    stream << "# TYPE " << name << " gauge\n" << name << ' '
           << std::fixed << std::setprecision(3) << value << '\n';
    stream << std::defaultfloat << std::setprecision(6);
}

void MetricsTextWriter::append_labeled_counter(const char* name,
                                               std::string_view labels,
                                               std::uint64_t value) const {
    stream << name << '{' << labels << "} " << value << '\n';
}

void MetricsTextWriter::append_labeled_gauge(const char* name,
                                             std::string_view labels,
                                             long double value) const {
    stream << name << '{' << labels << "} "
           << std::fixed << std::setprecision(3) << value << '\n';
    stream << std::defaultfloat << std::setprecision(6);
}

std::string escape_prometheus_label_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

void append_chat_bootstrap_metrics(MetricsTextWriter& writer, const BootstrapMetricsSnapshot& snapshot) {
    writer.append_counter("chat_subscribe_total", snapshot.subscribe_total);
    writer.append_counter("chat_self_echo_drop_total", snapshot.self_echo_drop_total);
    writer.append_gauge("chat_subscribe_last_lag_ms", static_cast<long double>(snapshot.subscribe_last_lag_ms));
    writer.append_counter("chat_admin_command_verify_ok_total", snapshot.admin_command_verify_ok_total);
    writer.append_counter("chat_admin_command_verify_fail_total", snapshot.admin_command_verify_fail_total);
    writer.append_counter("chat_admin_command_verify_replay_total", snapshot.admin_command_verify_replay_total);
    writer.append_counter(
        "chat_admin_command_verify_signature_mismatch_total",
        snapshot.admin_command_verify_signature_mismatch_total);
    writer.append_counter("chat_admin_command_verify_expired_total", snapshot.admin_command_verify_expired_total);
    writer.append_counter("chat_admin_command_verify_future_total", snapshot.admin_command_verify_future_total);
    writer.append_counter(
        "chat_admin_command_verify_missing_field_total",
        snapshot.admin_command_verify_missing_field_total);
    writer.append_counter(
        "chat_admin_command_verify_invalid_issued_at_total",
        snapshot.admin_command_verify_invalid_issued_at_total);
    writer.append_counter(
        "chat_admin_command_verify_secret_not_configured_total",
        snapshot.admin_command_verify_secret_not_configured_total);
    writer.append_counter(
        "chat_admin_command_target_mismatch_total",
        snapshot.admin_command_target_mismatch_total);
    writer.append_counter("chat_shutdown_drain_completed_total", snapshot.shutdown_drain_completed_total);
    writer.append_counter("chat_shutdown_drain_timeout_total", snapshot.shutdown_drain_timeout_total);
    writer.append_counter("chat_shutdown_drain_forced_close_total", snapshot.shutdown_drain_forced_close_total);
    writer.append_gauge(
        "chat_shutdown_drain_remaining_connections",
        static_cast<long double>(snapshot.shutdown_drain_remaining_connections));
    writer.append_gauge(
        "chat_shutdown_drain_elapsed_ms",
        static_cast<long double>(snapshot.shutdown_drain_elapsed_ms));
    writer.append_gauge(
        "chat_shutdown_drain_timeout_ms",
        static_cast<long double>(snapshot.shutdown_drain_timeout_ms));
}

void append_chat_continuity_metrics(MetricsTextWriter& writer,
                                    std::ostream& stream,
                                    const std::optional<server::app::chat::ContinuityMetrics>& snapshot) {
    server::app::chat::ContinuityMetrics continuity_metrics{};
    if (snapshot.has_value()) {
        continuity_metrics = *snapshot;
    }

    writer.append_counter("chat_continuity_lease_issue_total", continuity_metrics.lease_issue_total);
    writer.append_counter("chat_continuity_lease_issue_fail_total", continuity_metrics.lease_issue_fail_total);
    writer.append_counter("chat_continuity_lease_resume_total", continuity_metrics.lease_resume_total);
    writer.append_counter("chat_continuity_lease_resume_fail_total", continuity_metrics.lease_resume_fail_total);
    writer.append_counter("chat_continuity_state_write_total", continuity_metrics.state_write_total);
    writer.append_counter("chat_continuity_state_write_fail_total", continuity_metrics.state_write_fail_total);
    writer.append_counter("chat_continuity_state_restore_total", continuity_metrics.state_restore_total);
    writer.append_counter(
        "chat_continuity_state_restore_fallback_total",
        continuity_metrics.state_restore_fallback_total);
    writer.append_counter("chat_continuity_world_write_total", continuity_metrics.world_write_total);
    writer.append_counter("chat_continuity_world_write_fail_total", continuity_metrics.world_write_fail_total);
    writer.append_counter("chat_continuity_world_restore_total", continuity_metrics.world_restore_total);
    writer.append_counter(
        "chat_continuity_world_restore_fallback_total",
        continuity_metrics.world_restore_fallback_total);

    stream << "# TYPE chat_continuity_world_restore_fallback_reason_total counter\n";
    stream << "chat_continuity_world_restore_fallback_reason_total{reason=\"missing_world\"} "
           << continuity_metrics.world_restore_fallback_missing_world_total << "\n";
    stream << "chat_continuity_world_restore_fallback_reason_total{reason=\"missing_owner\"} "
           << continuity_metrics.world_restore_fallback_missing_owner_total << "\n";
    stream << "chat_continuity_world_restore_fallback_reason_total{reason=\"owner_mismatch\"} "
           << continuity_metrics.world_restore_fallback_owner_mismatch_total << "\n";
    stream
        << "chat_continuity_world_restore_fallback_reason_total{reason=\"draining_replacement_unhonored\"} "
        << continuity_metrics.world_restore_fallback_draining_replacement_unhonored_total << "\n";

    writer.append_counter("chat_continuity_world_owner_write_total", continuity_metrics.world_owner_write_total);
    writer.append_counter(
        "chat_continuity_world_owner_write_fail_total",
        continuity_metrics.world_owner_write_fail_total);
    writer.append_counter(
        "chat_continuity_world_owner_restore_total",
        continuity_metrics.world_owner_restore_total);
    writer.append_counter(
        "chat_continuity_world_owner_restore_fallback_total",
        continuity_metrics.world_owner_restore_fallback_total);
    writer.append_counter(
        "chat_continuity_world_migration_restore_total",
        continuity_metrics.world_migration_restore_total);
    writer.append_counter(
        "chat_continuity_world_migration_restore_fallback_total",
        continuity_metrics.world_migration_restore_fallback_total);

    stream << "# TYPE chat_continuity_world_migration_restore_fallback_reason_total counter\n";
    stream << "chat_continuity_world_migration_restore_fallback_reason_total{reason=\"target_world_missing\"} "
           << continuity_metrics.world_migration_restore_fallback_target_world_missing_total << "\n";
    stream << "chat_continuity_world_migration_restore_fallback_reason_total{reason=\"target_owner_missing\"} "
           << continuity_metrics.world_migration_restore_fallback_target_owner_missing_total << "\n";
    stream << "chat_continuity_world_migration_restore_fallback_reason_total{reason=\"target_owner_not_ready\"} "
           << continuity_metrics.world_migration_restore_fallback_target_owner_not_ready_total << "\n";
    stream << "chat_continuity_world_migration_restore_fallback_reason_total{reason=\"target_owner_mismatch\"} "
           << continuity_metrics.world_migration_restore_fallback_target_owner_mismatch_total << "\n";
    stream << "chat_continuity_world_migration_restore_fallback_reason_total{reason=\"source_not_draining\"} "
           << continuity_metrics.world_migration_restore_fallback_source_not_draining_total << "\n";
    writer.append_counter(
        "chat_continuity_world_migration_payload_room_handoff_total",
        continuity_metrics.world_migration_payload_room_handoff_total);
    writer.append_counter(
        "chat_continuity_world_migration_payload_room_handoff_fallback_total",
        continuity_metrics.world_migration_payload_room_handoff_fallback_total);
}

void append_chat_hook_plugin_metrics(std::ostream& stream,
                                     const std::optional<server::app::chat::ChatHookPluginsMetrics>& snapshot) {
    const MetricsTextWriter writer{stream};
    server::app::chat::ChatHookPluginsMetrics pm;
    bool have_pm = false;
    if (snapshot.has_value()) {
        pm = *snapshot;
        have_pm = true;
    } else {
        pm.enabled = false;
        pm.mode = "none";
    }

    stream << "# TYPE chat_hook_plugins_enabled gauge\n";
    stream << "chat_hook_plugins_enabled{mode=\"";
    stream << escape_prometheus_label_value(pm.mode);
    stream << "\"} " << (pm.enabled ? 1 : 0) << "\n";

    stream << "# TYPE chat_hook_plugins_count gauge\n";
    stream << "chat_hook_plugins_count " << static_cast<long double>(pm.plugins.size()) << "\n";

    std::size_t loaded = 0;
    for (const auto& plugin : pm.plugins) {
        if (plugin.loaded) {
            ++loaded;
        }
    }
    stream << "# TYPE chat_hook_plugins_loaded gauge\n";
    stream << "chat_hook_plugins_loaded " << static_cast<long double>(loaded) << "\n";

    if (!have_pm || pm.plugins.empty()) {
        return;
    }

    stream << "# TYPE chat_hook_plugin_loaded gauge\n";
    stream << "# TYPE chat_hook_plugin_order gauge\n";
    stream << "# TYPE chat_hook_plugin_info gauge\n";
    stream << "# TYPE chat_hook_plugin_reload_attempt_total counter\n";
    stream << "# TYPE chat_hook_plugin_reload_success_total counter\n";
    stream << "# TYPE chat_hook_plugin_reload_failure_total counter\n";
    stream << "# TYPE plugin_reload_attempt_total counter\n";
    stream << "# TYPE plugin_reload_success_total counter\n";
    stream << "# TYPE plugin_reload_failure_total counter\n";
    stream << "# TYPE plugin_hook_calls_total counter\n";
    stream << "# TYPE plugin_hook_errors_total counter\n";
    stream << "# TYPE plugin_hook_duration_seconds histogram\n";

    for (std::size_t i = 0; i < pm.plugins.size(); ++i) {
        const auto& plugin = pm.plugins[i];
        if (plugin.file.empty()) {
            continue;
        }

        const auto file = escape_prometheus_label_value(plugin.file);
        const std::string labels = std::string("file=\"") + file + "\"";
        writer.append_labeled_gauge("chat_hook_plugin_loaded", labels, plugin.loaded ? 1.0L : 0.0L);
        writer.append_labeled_gauge("chat_hook_plugin_order", labels, static_cast<long double>(i + 1));
        writer.append_labeled_counter(
            "chat_hook_plugin_reload_attempt_total", labels, plugin.reload_attempt_total);
        writer.append_labeled_counter(
            "chat_hook_plugin_reload_success_total", labels, plugin.reload_success_total);
        writer.append_labeled_counter(
            "chat_hook_plugin_reload_failure_total", labels, plugin.reload_failure_total);

        std::string plugin_name = plugin.name.empty() ? plugin.file : plugin.name;
        const std::string plugin_labels =
            std::string("plugin_name=\"") + escape_prometheus_label_value(plugin_name) + "\"";
        writer.append_labeled_counter("plugin_reload_attempt_total", plugin_labels, plugin.reload_attempt_total);
        writer.append_labeled_counter("plugin_reload_success_total", plugin_labels, plugin.reload_success_total);
        writer.append_labeled_counter("plugin_reload_failure_total", plugin_labels, plugin.reload_failure_total);

        for (const auto& hook_metric : plugin.hook_metrics) {
            const std::string hook_labels = std::string("hook_name=\"")
                                            + escape_prometheus_label_value(hook_metric.hook_name)
                                            + "\",plugin_name=\""
                                            + escape_prometheus_label_value(plugin_name)
                                            + "\"";
            writer.append_labeled_counter("plugin_hook_calls_total", hook_labels, hook_metric.calls_total);
            writer.append_labeled_counter("plugin_hook_errors_total", hook_labels, hook_metric.errors_total);

            std::uint64_t bucket_cumulative = 0;
            for (std::size_t bucket_index = 0;
                 bucket_index < kPluginHookDurationBucketUpperBoundsNs.size();
                 ++bucket_index) {
                bucket_cumulative += hook_metric.duration_bucket_counts[bucket_index];
                const long double le_seconds =
                    static_cast<long double>(kPluginHookDurationBucketUpperBoundsNs[bucket_index]) /
                    1'000'000'000.0L;

                stream << "plugin_hook_duration_seconds_bucket{" << hook_labels << ",le=\"";
                stream << std::fixed << std::setprecision(9) << le_seconds;
                stream << "\"} " << bucket_cumulative << "\n";
                stream << std::defaultfloat << std::setprecision(6);
            }

            stream << "plugin_hook_duration_seconds_bucket{" << hook_labels << ",le=\"+Inf\"} "
                   << hook_metric.duration_count << "\n";

            const long double duration_sum_seconds =
                static_cast<long double>(hook_metric.duration_sum_ns) / 1'000'000'000.0L;
            stream << "plugin_hook_duration_seconds_sum{" << hook_labels << "} ";
            stream << std::fixed << std::setprecision(9) << duration_sum_seconds << "\n";
            stream << std::defaultfloat << std::setprecision(6);
            stream << "plugin_hook_duration_seconds_count{" << hook_labels << "} "
                   << hook_metric.duration_count << "\n";
        }

        if (plugin.loaded) {
            std::string info_labels = labels;
            info_labels += ",name=\"" + escape_prometheus_label_value(plugin.name) + "\"";
            info_labels += ",version=\"" + escape_prometheus_label_value(plugin.version) + "\"";
            writer.append_labeled_gauge("chat_hook_plugin_info", info_labels, 1.0L);
        }
    }
}

void append_chat_lua_hook_metrics(std::ostream& stream,
                                  const std::optional<server::app::chat::LuaHooksMetrics>& snapshot) {
    server::app::chat::LuaHooksMetrics lua_metrics;
    bool have_lua_metrics = false;
    if (snapshot.has_value()) {
        lua_metrics = *snapshot;
        have_lua_metrics = true;
    }

    stream << "# TYPE hook_auto_disable_total counter\n";
    stream << "# TYPE chat_lua_hooks_enabled gauge\n";
    stream << "# TYPE chat_lua_hook_disabled gauge\n";
    stream << "# TYPE chat_lua_hook_consecutive_failures gauge\n";
    stream << "# TYPE chat_lua_hook_auto_disable_threshold gauge\n";
    stream << "# TYPE lua_loaded_scripts gauge\n";
    stream << "# TYPE lua_memory_used_bytes gauge\n";
    stream << "# TYPE lua_script_calls_total counter\n";
    stream << "# TYPE lua_script_errors_total counter\n";
    stream << "# TYPE lua_instruction_limit_hits_total counter\n";
    stream << "# TYPE lua_memory_limit_hits_total counter\n";

    stream << "chat_lua_hook_auto_disable_threshold "
           << static_cast<long double>(lua_metrics.auto_disable_threshold) << "\n";
    stream << "chat_lua_hooks_enabled " << (lua_metrics.enabled ? 1 : 0) << "\n";
    stream << "lua_loaded_scripts " << static_cast<long double>(lua_metrics.loaded_scripts) << "\n";
    stream << "lua_memory_used_bytes " << static_cast<long double>(lua_metrics.memory_used_bytes) << "\n";

    if (!have_lua_metrics) {
        return;
    }

    for (const auto& hook : lua_metrics.hooks) {
        const auto hook_name = escape_prometheus_label_value(hook.hook_name);
        const std::string labels = std::string("hook_name=\"") + hook_name + "\",source=\"lua\"";
        stream << "hook_auto_disable_total{" << labels << "} " << hook.auto_disable_total << "\n";
        stream << "chat_lua_hook_disabled{" << labels << "} " << (hook.disabled ? 1 : 0) << "\n";
        stream << "chat_lua_hook_consecutive_failures{" << labels << "} "
               << static_cast<long double>(hook.consecutive_failures) << "\n";
        stream << "lua_instruction_limit_hits_total{hook_name=\"" << hook_name << "\"} "
               << hook.instruction_limit_hits << "\n";
        stream << "lua_memory_limit_hits_total{hook_name=\"" << hook_name << "\"} "
               << hook.memory_limit_hits << "\n";
    }

    for (const auto& script_call : lua_metrics.script_calls) {
        const auto hook_name = escape_prometheus_label_value(script_call.hook_name);
        const auto script_name = escape_prometheus_label_value(script_call.script_name);
        stream << "lua_script_calls_total{hook_name=\"" << hook_name
               << "\",script_name=\"" << script_name << "\"} "
               << script_call.calls_total << "\n";
        stream << "lua_script_errors_total{hook_name=\"" << hook_name
               << "\",script_name=\"" << script_name << "\"} "
               << script_call.errors_total << "\n";
    }
}

} // namespace server::app
