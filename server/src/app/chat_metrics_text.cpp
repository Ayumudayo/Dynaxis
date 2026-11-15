#include "chat_metrics_text.hpp"

#include <array>
#include <iomanip>
#include <ostream>

namespace server::app {

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
}

} // namespace server::app
