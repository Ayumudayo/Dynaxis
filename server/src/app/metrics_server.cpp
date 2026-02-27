#include "server/app/metrics_server.hpp"

#include "server/chat/chat_service.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/service_registry.hpp"
#include "server/protocol/game_opcodes.hpp"

#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

/**
 * @brief server_app `/metrics`, `/healthz`, `/readyz` 렌더링 구현입니다.
 *
 * 런타임 카운터를 Prometheus 텍스트로 변환하고,
 * readiness/health 상태를 AppHost와 동기화해 오케스트레이터 판정을 일관화합니다.
 */
namespace server::app {

namespace corelog = server::core::log;
namespace services = server::core::util::services;

// 전역 메트릭 변수 (bootstrap.cpp에서 정의됨, extern으로 참조)
extern std::atomic<std::uint64_t> g_subscribe_total;
extern std::atomic<std::uint64_t> g_self_echo_drop_total;
extern std::atomic<long long>     g_subscribe_last_lag_ms;
extern std::atomic<std::uint64_t> g_admin_command_verify_ok_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_fail_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_replay_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_signature_mismatch_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_expired_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_future_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_missing_field_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_invalid_issued_at_total;
extern std::atomic<std::uint64_t> g_admin_command_verify_secret_not_configured_total;
extern std::atomic<std::uint64_t> g_shutdown_drain_completed_total;
extern std::atomic<std::uint64_t> g_shutdown_drain_timeout_total;
extern std::atomic<std::uint64_t> g_shutdown_drain_forced_close_total;
extern std::atomic<std::uint64_t> g_shutdown_drain_remaining_connections;
extern std::atomic<long long> g_shutdown_drain_elapsed_ms;
extern std::atomic<long long> g_shutdown_drain_timeout_ms;

namespace {

bool health_ok() {
    if (auto host = services::get<server::core::app::AppHost>()) {
        return host->healthy() && !host->stop_requested();
    }
    return true;
}

bool ready_ok() {
    if (auto host = services::get<server::core::app::AppHost>()) {
        return host->ready() && host->healthy() && !host->stop_requested();
    }
    return (services::get<server::app::chat::ChatService>() != nullptr);
}

std::string render_logs() {
    auto logs = corelog::recent(200);
    std::ostringstream body_stream;
    if (logs.empty()) {
        body_stream << "(no log entries)\n";
    } else {
        for (const auto& line : logs) {
            body_stream << line << '\n';
        }
    }
    return body_stream.str();
}

std::string render_metrics() {
    auto snap = server::core::runtime_metrics::snapshot();
    std::ostringstream stream;

    // Build metadata (git hash/describe + build time)
    server::core::metrics::append_build_info(stream);
    server::core::metrics::append_runtime_core_metrics(stream);
    server::core::metrics::append_prometheus_metrics(stream);

    auto append_counter = [&](const char* name, std::uint64_t value) {
        stream << "# TYPE " << name << " counter\n" << name << ' ' << value << '\n';
    };
    auto append_gauge = [&](const char* name, long double value) {
        stream << "# TYPE " << name << " gauge\n" << name << ' ' << std::fixed << std::setprecision(3) << value << '\n';
        stream << std::defaultfloat << std::setprecision(6);
    };

    append_counter("chat_subscribe_total", g_subscribe_total.load());
    append_counter("chat_self_echo_drop_total", g_self_echo_drop_total.load());
    append_gauge("chat_subscribe_last_lag_ms", static_cast<long double>(g_subscribe_last_lag_ms.load()));
    append_counter("chat_admin_command_verify_ok_total", g_admin_command_verify_ok_total.load());
    append_counter("chat_admin_command_verify_fail_total", g_admin_command_verify_fail_total.load());
    append_counter("chat_admin_command_verify_replay_total", g_admin_command_verify_replay_total.load());
    append_counter(
        "chat_admin_command_verify_signature_mismatch_total",
        g_admin_command_verify_signature_mismatch_total.load());
    append_counter("chat_admin_command_verify_expired_total", g_admin_command_verify_expired_total.load());
    append_counter("chat_admin_command_verify_future_total", g_admin_command_verify_future_total.load());
    append_counter("chat_admin_command_verify_missing_field_total", g_admin_command_verify_missing_field_total.load());
    append_counter(
        "chat_admin_command_verify_invalid_issued_at_total",
        g_admin_command_verify_invalid_issued_at_total.load());
    append_counter(
        "chat_admin_command_verify_secret_not_configured_total",
        g_admin_command_verify_secret_not_configured_total.load());
    append_counter("chat_shutdown_drain_completed_total", g_shutdown_drain_completed_total.load());
    append_counter("chat_shutdown_drain_timeout_total", g_shutdown_drain_timeout_total.load());
    append_counter("chat_shutdown_drain_forced_close_total", g_shutdown_drain_forced_close_total.load());
    append_gauge(
        "chat_shutdown_drain_remaining_connections",
        static_cast<long double>(g_shutdown_drain_remaining_connections.load()));
    append_gauge("chat_shutdown_drain_elapsed_ms", static_cast<long double>(g_shutdown_drain_elapsed_ms.load()));
    append_gauge("chat_shutdown_drain_timeout_ms", static_cast<long double>(g_shutdown_drain_timeout_ms.load()));

    append_counter("chat_accept_total", snap.accept_total);
    append_counter("chat_session_started_total", snap.session_started_total);
    append_counter("chat_session_stopped_total", snap.session_stopped_total);
    append_counter("chat_session_timeout_total", snap.session_timeout_total);
    append_counter("chat_session_write_timeout_total", snap.session_write_timeout_total);
    append_counter("chat_heartbeat_timeout_total", snap.heartbeat_timeout_total);
    append_counter("chat_send_queue_drop_total", snap.send_queue_drop_total);
    append_gauge("chat_session_active", static_cast<long double>(snap.session_active));
    append_counter("chat_frame_total", snap.packet_total);
    append_counter("chat_frame_error_total", snap.packet_error_total);
    append_counter("chat_frame_payload_sum_bytes", snap.packet_payload_sum_bytes);
    append_counter("chat_frame_payload_count", snap.packet_payload_count);
    auto payload_avg = snap.packet_payload_count ? (static_cast<long double>(snap.packet_payload_sum_bytes) / static_cast<long double>(snap.packet_payload_count)) : 0.0L;
    append_gauge("chat_frame_payload_avg_bytes", payload_avg);
    append_gauge("chat_frame_payload_max_bytes", static_cast<long double>(snap.packet_payload_max_bytes));
    append_counter("chat_dispatch_total", snap.dispatch_total);
    append_counter("chat_dispatch_unknown_total", snap.dispatch_unknown_total);
    append_counter("chat_dispatch_exception_total", snap.dispatch_exception_total);
    append_counter("chat_exception_recoverable_total", snap.exception_recoverable_total);
    append_counter("chat_exception_fatal_total", snap.exception_fatal_total);
    append_counter("chat_exception_ignored_total", snap.exception_ignored_total);

    stream << "# TYPE chat_dispatch_processing_place_calls_total counter\n";
    stream << "chat_dispatch_processing_place_calls_total{place=\"inline\"} "
           << snap.dispatch_processing_place_calls_total[0] << "\n";
    stream << "chat_dispatch_processing_place_calls_total{place=\"worker\"} "
           << snap.dispatch_processing_place_calls_total[1] << "\n";
    stream << "chat_dispatch_processing_place_calls_total{place=\"room_strand\"} "
           << snap.dispatch_processing_place_calls_total[2] << "\n";

    stream << "# TYPE chat_dispatch_processing_place_reject_total counter\n";
    stream << "chat_dispatch_processing_place_reject_total{place=\"inline\"} "
           << snap.dispatch_processing_place_reject_total[0] << "\n";
    stream << "chat_dispatch_processing_place_reject_total{place=\"worker\"} "
           << snap.dispatch_processing_place_reject_total[1] << "\n";
    stream << "chat_dispatch_processing_place_reject_total{place=\"room_strand\"} "
           << snap.dispatch_processing_place_reject_total[2] << "\n";

    stream << "# TYPE chat_dispatch_processing_place_exception_total counter\n";
    stream << "chat_dispatch_processing_place_exception_total{place=\"inline\"} "
           << snap.dispatch_processing_place_exception_total[0] << "\n";
    stream << "chat_dispatch_processing_place_exception_total{place=\"worker\"} "
           << snap.dispatch_processing_place_exception_total[1] << "\n";
    stream << "chat_dispatch_processing_place_exception_total{place=\"room_strand\"} "
           << snap.dispatch_processing_place_exception_total[2] << "\n";

    auto last_ms = static_cast<long double>(snap.dispatch_latency_last_ns) / 1'000'000.0L;
    auto max_ms = static_cast<long double>(snap.dispatch_latency_max_ns) / 1'000'000.0L;
    auto sum_ms = static_cast<long double>(snap.dispatch_latency_sum_ns) / 1'000'000.0L;
    auto avg_ms = snap.dispatch_latency_count ? (sum_ms / static_cast<long double>(snap.dispatch_latency_count)) : 0.0L;
    append_gauge("chat_dispatch_last_latency_ms", last_ms);
    append_gauge("chat_dispatch_max_latency_ms", max_ms);
    append_gauge("chat_dispatch_latency_sum_ms", sum_ms);
    append_gauge("chat_dispatch_latency_avg_ms", avg_ms);
    append_counter("chat_dispatch_latency_count", snap.dispatch_latency_count);

    // Dispatch latency histogram (for p95/p99 etc.)
    stream << "# TYPE chat_dispatch_latency_ms histogram\n";
    std::uint64_t bucket_cumulative = 0;
    for (std::size_t i = 0; i < snap.dispatch_latency_bucket_counts.size(); ++i) {
        bucket_cumulative += snap.dispatch_latency_bucket_counts[i];
        auto le_ms = static_cast<long double>(server::core::runtime_metrics::kDispatchLatencyBucketUpperBoundsNs[i]) / 1'000'000.0L;
        stream << "chat_dispatch_latency_ms_bucket{le=\"";
        stream << std::fixed << std::setprecision(3) << le_ms;
        stream << "\"} " << bucket_cumulative << "\n";
        stream << std::defaultfloat << std::setprecision(6);
    }
    stream << "chat_dispatch_latency_ms_bucket{le=\"+Inf\"} " << snap.dispatch_latency_count << "\n";
    stream << "chat_dispatch_latency_ms_sum " << std::fixed << std::setprecision(3) << sum_ms << "\n";
    stream << std::defaultfloat << std::setprecision(6);
    stream << "chat_dispatch_latency_ms_count " << snap.dispatch_latency_count << "\n";
    append_gauge("chat_job_queue_depth", static_cast<long double>(snap.job_queue_depth));
    append_gauge("chat_job_queue_depth_peak", static_cast<long double>(snap.job_queue_depth_peak));
    append_gauge("chat_job_queue_capacity", static_cast<long double>(snap.job_queue_capacity));
    append_counter("chat_job_queue_reject_total", snap.job_queue_reject_total);
    append_counter("chat_job_queue_push_wait_ns_total", snap.job_queue_push_wait_sum_ns);
    append_counter("chat_job_queue_push_wait_total", snap.job_queue_push_wait_count);
    append_gauge("chat_job_queue_push_wait_max_ms", static_cast<long double>(snap.job_queue_push_wait_max_ns) / 1'000'000.0L);
    append_gauge("chat_db_job_queue_depth", static_cast<long double>(snap.db_job_queue_depth));
    append_gauge("chat_db_job_queue_depth_peak", static_cast<long double>(snap.db_job_queue_depth_peak));
    append_gauge("chat_db_job_queue_capacity", static_cast<long double>(snap.db_job_queue_capacity));
    append_counter("chat_db_job_queue_reject_total", snap.db_job_queue_reject_total);
    append_counter("chat_db_job_queue_push_wait_ns_total", snap.db_job_queue_push_wait_sum_ns);
    append_counter("chat_db_job_queue_push_wait_total", snap.db_job_queue_push_wait_count);
    append_gauge("chat_db_job_queue_push_wait_max_ms", static_cast<long double>(snap.db_job_queue_push_wait_max_ns) / 1'000'000.0L);
    append_counter("chat_db_job_processed_total", snap.db_job_processed_total);
    append_counter("chat_db_job_failed_total", snap.db_job_failed_total);
    append_gauge("chat_memory_pool_capacity", static_cast<long double>(snap.memory_pool_capacity));
    append_gauge("chat_memory_pool_in_use", static_cast<long double>(snap.memory_pool_in_use));
    append_gauge("chat_memory_pool_in_use_peak", static_cast<long double>(snap.memory_pool_in_use_peak));
    append_gauge("chat_log_async_queue_depth", static_cast<long double>(snap.log_async_queue_depth));
    append_gauge("chat_log_async_queue_capacity", static_cast<long double>(snap.log_async_queue_capacity));
    append_counter("chat_log_async_queue_drop_total", snap.log_async_queue_drop_total);
    append_counter("chat_log_async_flush_total", snap.log_async_flush_total);
    append_counter("chat_log_async_flush_latency_sum_ns", snap.log_async_flush_latency_sum_ns);
    append_counter("chat_log_masked_fields_total", snap.log_masked_fields_total);
    append_gauge("chat_log_async_flush_latency_max_ms", static_cast<long double>(snap.log_async_flush_latency_max_ns) / 1'000'000.0L);
    append_gauge("chat_http_active_connections", static_cast<long double>(snap.http_active_connections));
    append_counter("chat_http_connection_limit_reject_total", snap.http_connection_limit_reject_total);
    append_counter("chat_http_auth_reject_total", snap.http_auth_reject_total);
    append_counter("chat_http_header_timeout_total", snap.http_header_timeout_total);
    append_counter("chat_http_body_timeout_total", snap.http_body_timeout_total);
    append_counter("chat_http_header_oversize_total", snap.http_header_oversize_total);
    append_counter("chat_http_body_oversize_total", snap.http_body_oversize_total);
    append_counter("chat_http_bad_request_total", snap.http_bad_request_total);
    append_counter("chat_runtime_setting_reload_attempt_total", snap.runtime_setting_reload_attempt_total);
    append_counter("chat_runtime_setting_reload_success_total", snap.runtime_setting_reload_success_total);
    append_counter("chat_runtime_setting_reload_failure_total", snap.runtime_setting_reload_failure_total);
    append_counter("chat_runtime_setting_reload_latency_sum_ns", snap.runtime_setting_reload_latency_sum_ns);
    append_gauge(
        "chat_runtime_setting_reload_latency_max_ms",
        static_cast<long double>(snap.runtime_setting_reload_latency_max_ns) / 1'000'000.0L);

    if (!snap.opcode_counts.empty()) {
        stream << "# TYPE chat_dispatch_opcode_total counter\n";
        for (const auto& [opcode, count] : snap.opcode_counts) {
            std::ostringstream label;
            label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
            stream << "chat_dispatch_opcode_total{opcode=\"0x" << label.str() << "\"} " << count << "\n";
        }

        // Same counter, but with stable, human-readable opcode names.
        // Keep the original metric intact to avoid breaking existing dashboards.
        stream << "# TYPE chat_dispatch_opcode_named_total counter\n";
        for (const auto& [opcode, count] : snap.opcode_counts) {
            std::string_view name = server::protocol::opcode_name(opcode);
            if (name.empty()) {
                name = server::core::protocol::opcode_name(opcode);
            }
            if (name.empty()) {
                name = "unknown";
            }

            std::ostringstream label;
            label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
            stream << "chat_dispatch_opcode_named_total{opcode=\"0x" << label.str() << "\",name=\"" << name << "\"} " << count << "\n";
        }
    }

    // Chat hook plugins (hot-reloadable shared libraries)
    {
        const auto escape_label_value = [](std::string_view s) {
            std::string out;
            out.reserve(s.size());
            for (const char c : s) {
                switch (c) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                default: out.push_back(c); break;
                }
            }
            return out;
        };

        const auto append_gauge_labeled = [&](const char* name,
                                              std::string_view labels,
                                              long double value) {
            stream << name << '{' << labels << "} ";
            stream << std::fixed << std::setprecision(3) << value << "\n";
            stream << std::defaultfloat << std::setprecision(6);
        };

        const auto append_counter_labeled = [&](const char* name,
                                                std::string_view labels,
                                                std::uint64_t value) {
            stream << name << '{' << labels << "} " << value << "\n";
        };

        server::app::chat::ChatService::ChatHookPluginsMetrics pm;
        bool have_pm = false;
        if (auto chat = services::get<server::app::chat::ChatService>()) {
            pm = chat->chat_hook_plugins_metrics();
            have_pm = true;
        } else {
            pm.enabled = false;
            pm.mode = "none";
        }

        stream << "# TYPE chat_hook_plugins_enabled gauge\n";
        stream << "chat_hook_plugins_enabled{mode=\"";
        stream << escape_label_value(pm.mode);
        stream << "\"} " << (pm.enabled ? 1 : 0) << "\n";

        stream << "# TYPE chat_hook_plugins_count gauge\n";
        stream << "chat_hook_plugins_count " << static_cast<long double>(pm.plugins.size()) << "\n";

        std::size_t loaded = 0;
        for (const auto& p : pm.plugins) {
            if (p.loaded) {
                ++loaded;
            }
        }
        stream << "# TYPE chat_hook_plugins_loaded gauge\n";
        stream << "chat_hook_plugins_loaded " << static_cast<long double>(loaded) << "\n";

        if (have_pm && !pm.plugins.empty()) {
            stream << "# TYPE chat_hook_plugin_loaded gauge\n";
            stream << "# TYPE chat_hook_plugin_order gauge\n";
            stream << "# TYPE chat_hook_plugin_info gauge\n";
            stream << "# TYPE chat_hook_plugin_reload_attempt_total counter\n";
            stream << "# TYPE chat_hook_plugin_reload_success_total counter\n";
            stream << "# TYPE chat_hook_plugin_reload_failure_total counter\n";

            for (std::size_t i = 0; i < pm.plugins.size(); ++i) {
                const auto& p = pm.plugins[i];
                if (p.file.empty()) {
                    continue;
                }

                const auto file = escape_label_value(p.file);
                std::string labels = std::string("file=\"") + file + "\"";

                append_gauge_labeled("chat_hook_plugin_loaded", labels, p.loaded ? 1.0L : 0.0L);
                append_gauge_labeled("chat_hook_plugin_order", labels, static_cast<long double>(i + 1));

                append_counter_labeled("chat_hook_plugin_reload_attempt_total", labels, p.reload_attempt_total);
                append_counter_labeled("chat_hook_plugin_reload_success_total", labels, p.reload_success_total);
                append_counter_labeled("chat_hook_plugin_reload_failure_total", labels, p.reload_failure_total);

                if (p.loaded) {
                    std::string info_labels = labels;
                    info_labels += ",name=\"" + escape_label_value(p.name) + "\"";
                    info_labels += ",version=\"" + escape_label_value(p.version) + "\"";
                    append_gauge_labeled("chat_hook_plugin_info", info_labels, 1.0L);
                }
            }
        }
    }

    if (const auto host = services::get<server::core::app::AppHost>()) {
        stream << host->dependency_metrics_text();
        stream << host->lifecycle_metrics_text();
    }

    stream << std::setfill(' ') << std::dec << std::nouppercase;
    return stream.str();
}

} // namespace

MetricsServer::MetricsServer(unsigned short port)
    : port_(port) {
}

MetricsServer::~MetricsServer() {
    stop();
}

void MetricsServer::start() {
    if (http_server_) {
        return;
    }

    http_server_ = std::make_unique<server::core::metrics::MetricsHttpServer>(
        port_,
        []() { return render_metrics(); },
        []() { return health_ok(); },
        []() { return ready_ok(); },
        []() { return render_logs(); },
        [](bool ok) {
            if (const auto host = services::get<server::core::app::AppHost>()) {
                return host->health_body(ok);
            }
            return ok ? std::string("ok\n") : std::string("unhealthy\n");
        },
        [](bool ok) {
            if (const auto host = services::get<server::core::app::AppHost>()) {
                return host->readiness_body(ok);
            }
            return ok ? std::string("ready\n") : std::string("not ready\n");
        });
    http_server_->start();
}

void MetricsServer::stop() {
    if (!http_server_) {
        return;
    }
    http_server_->stop();
    http_server_.reset();
}

} // namespace server::app
