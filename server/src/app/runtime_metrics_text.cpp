#include "runtime_metrics_text.hpp"

#include "server/core/protocol/system_opcodes.hpp"
#include "server/protocol/game_opcodes.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string_view>

namespace server::app {

namespace {

void append_processing_place_metrics(std::ostream& stream,
                                     const server::core::runtime_metrics::Snapshot& snapshot) {
    stream << "# TYPE chat_dispatch_processing_place_calls_total counter\n";
    stream << "chat_dispatch_processing_place_calls_total{place=\"inline\"} "
           << snapshot.dispatch_processing_place_calls_total[0] << "\n";
    stream << "chat_dispatch_processing_place_calls_total{place=\"worker\"} "
           << snapshot.dispatch_processing_place_calls_total[1] << "\n";
    stream << "chat_dispatch_processing_place_calls_total{place=\"room_strand\"} "
           << snapshot.dispatch_processing_place_calls_total[2] << "\n";

    stream << "# TYPE chat_dispatch_processing_place_reject_total counter\n";
    stream << "chat_dispatch_processing_place_reject_total{place=\"inline\"} "
           << snapshot.dispatch_processing_place_reject_total[0] << "\n";
    stream << "chat_dispatch_processing_place_reject_total{place=\"worker\"} "
           << snapshot.dispatch_processing_place_reject_total[1] << "\n";
    stream << "chat_dispatch_processing_place_reject_total{place=\"room_strand\"} "
           << snapshot.dispatch_processing_place_reject_total[2] << "\n";

    stream << "# TYPE chat_dispatch_processing_place_exception_total counter\n";
    stream << "chat_dispatch_processing_place_exception_total{place=\"inline\"} "
           << snapshot.dispatch_processing_place_exception_total[0] << "\n";
    stream << "chat_dispatch_processing_place_exception_total{place=\"worker\"} "
           << snapshot.dispatch_processing_place_exception_total[1] << "\n";
    stream << "chat_dispatch_processing_place_exception_total{place=\"room_strand\"} "
           << snapshot.dispatch_processing_place_exception_total[2] << "\n";
}

void append_dispatch_latency_metrics(MetricsTextWriter& writer,
                                     std::ostream& stream,
                                     const server::core::runtime_metrics::Snapshot& snapshot) {
    const auto last_ms = static_cast<long double>(snapshot.dispatch_latency_last_ns) / 1'000'000.0L;
    const auto max_ms = static_cast<long double>(snapshot.dispatch_latency_max_ns) / 1'000'000.0L;
    const auto sum_ms = static_cast<long double>(snapshot.dispatch_latency_sum_ns) / 1'000'000.0L;
    const auto avg_ms = snapshot.dispatch_latency_count
        ? (sum_ms / static_cast<long double>(snapshot.dispatch_latency_count))
        : 0.0L;
    writer.append_gauge("chat_dispatch_last_latency_ms", last_ms);
    writer.append_gauge("chat_dispatch_max_latency_ms", max_ms);
    writer.append_gauge("chat_dispatch_latency_sum_ms", sum_ms);
    writer.append_gauge("chat_dispatch_latency_avg_ms", avg_ms);
    writer.append_counter("chat_dispatch_latency_count", snapshot.dispatch_latency_count);

    stream << "# TYPE chat_dispatch_latency_ms histogram\n";
    std::uint64_t bucket_cumulative = 0;
    for (std::size_t i = 0; i < snapshot.dispatch_latency_bucket_counts.size(); ++i) {
        bucket_cumulative += snapshot.dispatch_latency_bucket_counts[i];
        const auto le_ms =
            static_cast<long double>(server::core::runtime_metrics::kDispatchLatencyBucketUpperBoundsNs[i])
            / 1'000'000.0L;
        stream << "chat_dispatch_latency_ms_bucket{le=\"";
        stream << std::fixed << std::setprecision(3) << le_ms;
        stream << "\"} " << bucket_cumulative << "\n";
        stream << std::defaultfloat << std::setprecision(6);
    }
    stream << "chat_dispatch_latency_ms_bucket{le=\"+Inf\"} " << snapshot.dispatch_latency_count << "\n";
    stream << "chat_dispatch_latency_ms_sum " << std::fixed << std::setprecision(3) << sum_ms << "\n";
    stream << std::defaultfloat << std::setprecision(6);
    stream << "chat_dispatch_latency_ms_count " << snapshot.dispatch_latency_count << "\n";
}

void append_opcode_metrics(std::ostream& stream,
                           const server::core::runtime_metrics::Snapshot& snapshot) {
    if (snapshot.opcode_counts.empty()) {
        return;
    }

    stream << "# TYPE chat_dispatch_opcode_total counter\n";
    for (const auto& [opcode, count] : snapshot.opcode_counts) {
        std::ostringstream label;
        label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
        stream << "chat_dispatch_opcode_total{opcode=\"0x" << label.str() << "\"} " << count << "\n";
    }

    stream << "# TYPE chat_dispatch_opcode_named_total counter\n";
    for (const auto& [opcode, count] : snapshot.opcode_counts) {
        std::string_view name = server::protocol::opcode_name(opcode);
        if (name.empty()) {
            name = server::core::protocol::opcode_name(opcode);
        }
        if (name.empty()) {
            name = "unknown";
        }

        std::ostringstream label;
        label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
        stream << "chat_dispatch_opcode_named_total{opcode=\"0x" << label.str()
               << "\",name=\"" << name << "\"} " << count << "\n";
    }
}

} // namespace

void append_chat_runtime_metrics(MetricsTextWriter& writer,
                                 std::ostream& stream,
                                 const server::core::runtime_metrics::Snapshot& snapshot) {
    writer.append_counter("chat_accept_total", snapshot.accept_total);
    writer.append_counter("chat_session_started_total", snapshot.session_started_total);
    writer.append_counter("chat_session_stopped_total", snapshot.session_stopped_total);
    writer.append_counter("chat_session_timeout_total", snapshot.session_timeout_total);
    writer.append_counter("chat_session_write_timeout_total", snapshot.session_write_timeout_total);
    writer.append_counter("chat_heartbeat_timeout_total", snapshot.heartbeat_timeout_total);
    writer.append_counter("chat_send_queue_drop_total", snapshot.send_queue_drop_total);
    writer.append_gauge("chat_session_active", static_cast<long double>(snapshot.session_active));
    writer.append_counter("chat_frame_total", snapshot.packet_total);
    writer.append_counter("chat_frame_error_total", snapshot.packet_error_total);
    writer.append_counter("chat_frame_payload_sum_bytes", snapshot.packet_payload_sum_bytes);
    writer.append_counter("chat_frame_payload_count", snapshot.packet_payload_count);
    const auto payload_avg = snapshot.packet_payload_count
        ? (static_cast<long double>(snapshot.packet_payload_sum_bytes)
           / static_cast<long double>(snapshot.packet_payload_count))
        : 0.0L;
    writer.append_gauge("chat_frame_payload_avg_bytes", payload_avg);
    writer.append_gauge("chat_frame_payload_max_bytes", static_cast<long double>(snapshot.packet_payload_max_bytes));
    writer.append_counter("chat_dispatch_total", snapshot.dispatch_total);
    writer.append_counter("chat_dispatch_unknown_total", snapshot.dispatch_unknown_total);
    writer.append_counter("chat_dispatch_exception_total", snapshot.dispatch_exception_total);
    writer.append_counter("chat_exception_recoverable_total", snapshot.exception_recoverable_total);
    writer.append_counter("chat_exception_fatal_total", snapshot.exception_fatal_total);
    writer.append_counter("chat_exception_ignored_total", snapshot.exception_ignored_total);

    append_processing_place_metrics(stream, snapshot);
    append_dispatch_latency_metrics(writer, stream, snapshot);

    writer.append_gauge("chat_job_queue_depth", static_cast<long double>(snapshot.job_queue_depth));
    writer.append_gauge("chat_job_queue_depth_peak", static_cast<long double>(snapshot.job_queue_depth_peak));
    writer.append_gauge("chat_job_queue_capacity", static_cast<long double>(snapshot.job_queue_capacity));
    writer.append_counter("chat_job_queue_reject_total", snapshot.job_queue_reject_total);
    writer.append_counter("chat_job_queue_push_wait_ns_total", snapshot.job_queue_push_wait_sum_ns);
    writer.append_counter("chat_job_queue_push_wait_total", snapshot.job_queue_push_wait_count);
    writer.append_gauge(
        "chat_job_queue_push_wait_max_ms",
        static_cast<long double>(snapshot.job_queue_push_wait_max_ns) / 1'000'000.0L);
    writer.append_gauge("chat_db_job_queue_depth", static_cast<long double>(snapshot.db_job_queue_depth));
    writer.append_gauge("chat_db_job_queue_depth_peak", static_cast<long double>(snapshot.db_job_queue_depth_peak));
    writer.append_gauge("chat_db_job_queue_capacity", static_cast<long double>(snapshot.db_job_queue_capacity));
    writer.append_counter("chat_db_job_queue_reject_total", snapshot.db_job_queue_reject_total);
    writer.append_counter("chat_db_job_queue_push_wait_ns_total", snapshot.db_job_queue_push_wait_sum_ns);
    writer.append_counter("chat_db_job_queue_push_wait_total", snapshot.db_job_queue_push_wait_count);
    writer.append_gauge(
        "chat_db_job_queue_push_wait_max_ms",
        static_cast<long double>(snapshot.db_job_queue_push_wait_max_ns) / 1'000'000.0L);
    writer.append_counter("chat_db_job_processed_total", snapshot.db_job_processed_total);
    writer.append_counter("chat_db_job_failed_total", snapshot.db_job_failed_total);
    writer.append_gauge("chat_memory_pool_capacity", static_cast<long double>(snapshot.memory_pool_capacity));
    writer.append_gauge("chat_memory_pool_in_use", static_cast<long double>(snapshot.memory_pool_in_use));
    writer.append_gauge("chat_memory_pool_in_use_peak", static_cast<long double>(snapshot.memory_pool_in_use_peak));
    writer.append_gauge("chat_log_async_queue_depth", static_cast<long double>(snapshot.log_async_queue_depth));
    writer.append_gauge("chat_log_async_queue_capacity", static_cast<long double>(snapshot.log_async_queue_capacity));
    writer.append_counter("chat_log_async_queue_drop_total", snapshot.log_async_queue_drop_total);
    writer.append_counter("chat_log_async_flush_total", snapshot.log_async_flush_total);
    writer.append_counter("chat_log_async_flush_latency_sum_ns", snapshot.log_async_flush_latency_sum_ns);
    writer.append_counter("chat_log_masked_fields_total", snapshot.log_masked_fields_total);
    writer.append_gauge(
        "chat_log_async_flush_latency_max_ms",
        static_cast<long double>(snapshot.log_async_flush_latency_max_ns) / 1'000'000.0L);
    writer.append_gauge("chat_http_active_connections", static_cast<long double>(snapshot.http_active_connections));
    writer.append_counter("chat_http_connection_limit_reject_total", snapshot.http_connection_limit_reject_total);
    writer.append_counter("chat_http_auth_reject_total", snapshot.http_auth_reject_total);
    writer.append_counter("chat_http_header_timeout_total", snapshot.http_header_timeout_total);
    writer.append_counter("chat_http_body_timeout_total", snapshot.http_body_timeout_total);
    writer.append_counter("chat_http_header_oversize_total", snapshot.http_header_oversize_total);
    writer.append_counter("chat_http_body_oversize_total", snapshot.http_body_oversize_total);
    writer.append_counter("chat_http_bad_request_total", snapshot.http_bad_request_total);
    writer.append_counter("chat_runtime_setting_reload_attempt_total", snapshot.runtime_setting_reload_attempt_total);
    writer.append_counter("chat_runtime_setting_reload_success_total", snapshot.runtime_setting_reload_success_total);
    writer.append_counter("chat_runtime_setting_reload_failure_total", snapshot.runtime_setting_reload_failure_total);
    writer.append_counter("chat_runtime_setting_reload_latency_sum_ns", snapshot.runtime_setting_reload_latency_sum_ns);
    writer.append_gauge(
        "chat_runtime_setting_reload_latency_max_ms",
        static_cast<long double>(snapshot.runtime_setting_reload_latency_max_ns) / 1'000'000.0L);
    writer.append_gauge("chat_runtime_watchdog_total", static_cast<long double>(snapshot.watchdog_total));
    writer.append_gauge("chat_runtime_watchdog_unhealthy_total", static_cast<long double>(snapshot.watchdog_unhealthy_total));
    writer.append_counter("chat_runtime_watchdog_transition_total", snapshot.watchdog_transition_total);
    writer.append_counter("chat_runtime_watchdog_freeze_suspect_total", snapshot.watchdog_freeze_suspect_total);
    writer.append_counter(
        "chat_runtime_detailed_telemetry_activation_total",
        snapshot.detailed_telemetry_activation_total);
    writer.append_gauge(
        "chat_runtime_detailed_telemetry_active",
        snapshot.detailed_telemetry_active ? 1.0L : 0.0L);
    writer.append_gauge(
        "chat_runtime_detailed_telemetry_capture_budget_remaining",
        static_cast<long double>(snapshot.detailed_telemetry_capture_budget_remaining));
    writer.append_counter(
        "chat_runtime_detailed_telemetry_captured_exception_total",
        snapshot.detailed_telemetry_captured_exception_total);
    writer.append_gauge(
        "chat_runtime_detailed_telemetry_captured_dispatch_latency_max_ms",
        static_cast<long double>(snapshot.detailed_telemetry_captured_dispatch_latency_max_ns) / 1'000'000.0L);

    append_opcode_metrics(stream, snapshot);
}

void append_server_runtime_module_metrics(
    MetricsTextWriter& writer,
    std::ostream& stream,
    const std::optional<server::core::app::EngineRuntime::Snapshot>& runtime_snapshot,
    const std::vector<server::core::app::EngineRuntime::ModuleSnapshot>& modules) {
    if (!runtime_snapshot.has_value()) {
        return;
    }

    writer.append_gauge(
        "chat_runtime_context_service_count",
        static_cast<long double>(runtime_snapshot->context_service_count));
    writer.append_gauge(
        "chat_runtime_compatibility_bridge_count",
        static_cast<long double>(runtime_snapshot->compatibility_bridge_count));
    writer.append_gauge(
        "chat_runtime_registered_module_count",
        static_cast<long double>(runtime_snapshot->registered_module_count));
    writer.append_gauge(
        "chat_runtime_started_module_count",
        static_cast<long double>(runtime_snapshot->started_module_count));
    writer.append_gauge(
        "chat_runtime_module_watchdog_count",
        static_cast<long double>(runtime_snapshot->watchdog_count));
    writer.append_gauge(
        "chat_runtime_module_unhealthy_watchdog_count",
        static_cast<long double>(runtime_snapshot->unhealthy_watchdog_count));

    if (modules.empty()) {
        return;
    }

    stream << "# TYPE chat_runtime_module_started gauge\n";
    stream << "# TYPE chat_runtime_module_watchdog_healthy gauge\n";
    for (const auto& module : modules) {
        const std::string labels =
            std::string("module=\"") + escape_prometheus_label_value(module.name) + "\"";
        writer.append_labeled_gauge(
            "chat_runtime_module_started",
            labels,
            module.started ? 1.0L : 0.0L);
        writer.append_labeled_gauge(
            "chat_runtime_module_watchdog_healthy",
            labels,
            (!module.has_watchdog || module.watchdog_healthy) ? 1.0L : 0.0L);
    }
}

} // namespace server::app
