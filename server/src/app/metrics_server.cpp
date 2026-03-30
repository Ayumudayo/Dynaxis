#include "server/app/metrics_server.hpp"
#include "server_runtime_state.hpp"

#include "server/chat/chat_service.hpp"
#include "server/core/app/engine_runtime.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"
#include "server/protocol/game_opcodes.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

/**
 * @brief `server_app`의 `/metrics`, `/healthz`, `/readyz` 렌더링 구현입니다.
 *
 * 런타임 카운터를 Prometheus 텍스트로 변환하고,
 * 준비 상태(readiness)와 health 상태를 AppHost와 동기화해 오케스트레이터 판정을 일관화합니다.
 * 이 파일이 별도로 존재해야 메트릭 이름, probe 의미, 서비스별 추가 계수기를
 * 한곳에서 관리할 수 있습니다. 각 기능 모듈이 자기 메트릭을 제각각 문자열로 출력하면
 * 대시보드와 알람 규칙이 서비스 개편 때마다 쉽게 깨집니다.
 */
namespace server::app {

namespace corelog = server::core::log;
namespace {

constexpr std::array<std::uint64_t, 12> kPluginHookDurationBucketUpperBoundsNs{{
    1'000,      // 1us
    2'000,      // 2us
    5'000,      // 5us
    10'000,     // 10us
    25'000,     // 25us
    50'000,     // 50us
    100'000,    // 100us
    250'000,    // 250us
    500'000,    // 500us
    1'000'000,  // 1ms
    2'500'000,  // 2.5ms
    5'000'000,  // 5ms
}};

std::string render_logs_impl() {
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

std::string render_metrics_impl() {
    auto snap = server::core::runtime_metrics::snapshot();
    std::ostringstream stream;

    // 빌드 메타데이터(git hash/describe + build time)를 같이 내보내야
    // 같은 메트릭 이름을 보더라도 어느 바이너리에서 나온 값인지 현장에서 바로 구분할 수 있다.
    // 이 표식이 없으면 롤링 배포 중 "이상한 수치가 어느 버전에서 나온 것인가"를 추적하기가 급격히 어려워진다.
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
    const auto escape_label_value = [](std::string_view value) {
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
    };
    auto append_labeled_gauge = [&](const char* name, std::string_view labels, long double value) {
        stream << name << '{' << labels << "} " << std::fixed << std::setprecision(3) << value << '\n';
        stream << std::defaultfloat << std::setprecision(6);
    };

    const auto bootstrap_metrics = bootstrap_metrics_snapshot();
    append_counter("chat_subscribe_total", bootstrap_metrics.subscribe_total);
    append_counter("chat_self_echo_drop_total", bootstrap_metrics.self_echo_drop_total);
    append_gauge("chat_subscribe_last_lag_ms", static_cast<long double>(bootstrap_metrics.subscribe_last_lag_ms));
    append_counter("chat_admin_command_verify_ok_total", bootstrap_metrics.admin_command_verify_ok_total);
    append_counter("chat_admin_command_verify_fail_total", bootstrap_metrics.admin_command_verify_fail_total);
    append_counter("chat_admin_command_verify_replay_total", bootstrap_metrics.admin_command_verify_replay_total);
    append_counter(
        "chat_admin_command_verify_signature_mismatch_total",
        bootstrap_metrics.admin_command_verify_signature_mismatch_total);
    append_counter("chat_admin_command_verify_expired_total", bootstrap_metrics.admin_command_verify_expired_total);
    append_counter("chat_admin_command_verify_future_total", bootstrap_metrics.admin_command_verify_future_total);
    append_counter("chat_admin_command_verify_missing_field_total", bootstrap_metrics.admin_command_verify_missing_field_total);
    append_counter(
        "chat_admin_command_verify_invalid_issued_at_total",
        bootstrap_metrics.admin_command_verify_invalid_issued_at_total);
    append_counter(
        "chat_admin_command_verify_secret_not_configured_total",
        bootstrap_metrics.admin_command_verify_secret_not_configured_total);
    append_counter(
        "chat_admin_command_target_mismatch_total",
        bootstrap_metrics.admin_command_target_mismatch_total);
    append_counter("chat_shutdown_drain_completed_total", bootstrap_metrics.shutdown_drain_completed_total);
    append_counter("chat_shutdown_drain_timeout_total", bootstrap_metrics.shutdown_drain_timeout_total);
    append_counter("chat_shutdown_drain_forced_close_total", bootstrap_metrics.shutdown_drain_forced_close_total);
    append_gauge(
        "chat_shutdown_drain_remaining_connections",
        static_cast<long double>(bootstrap_metrics.shutdown_drain_remaining_connections));
    append_gauge("chat_shutdown_drain_elapsed_ms", static_cast<long double>(bootstrap_metrics.shutdown_drain_elapsed_ms));
    append_gauge("chat_shutdown_drain_timeout_ms", static_cast<long double>(bootstrap_metrics.shutdown_drain_timeout_ms));

    server::app::chat::ContinuityMetrics continuity_metrics{};
    if (const auto continuity_metrics_snapshot = server_chat_continuity_metrics()) {
        continuity_metrics = *continuity_metrics_snapshot;
    }
    append_counter("chat_continuity_lease_issue_total", continuity_metrics.lease_issue_total);
    append_counter("chat_continuity_lease_issue_fail_total", continuity_metrics.lease_issue_fail_total);
    append_counter("chat_continuity_lease_resume_total", continuity_metrics.lease_resume_total);
    append_counter("chat_continuity_lease_resume_fail_total", continuity_metrics.lease_resume_fail_total);
    append_counter("chat_continuity_state_write_total", continuity_metrics.state_write_total);
    append_counter("chat_continuity_state_write_fail_total", continuity_metrics.state_write_fail_total);
    append_counter("chat_continuity_state_restore_total", continuity_metrics.state_restore_total);
    append_counter(
        "chat_continuity_state_restore_fallback_total",
        continuity_metrics.state_restore_fallback_total);
    append_counter("chat_continuity_world_write_total", continuity_metrics.world_write_total);
    append_counter("chat_continuity_world_write_fail_total", continuity_metrics.world_write_fail_total);
    append_counter("chat_continuity_world_restore_total", continuity_metrics.world_restore_total);
    append_counter(
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
    append_counter("chat_continuity_world_owner_write_total", continuity_metrics.world_owner_write_total);
    append_counter(
        "chat_continuity_world_owner_write_fail_total",
        continuity_metrics.world_owner_write_fail_total);
    append_counter(
        "chat_continuity_world_owner_restore_total",
        continuity_metrics.world_owner_restore_total);
    append_counter(
        "chat_continuity_world_owner_restore_fallback_total",
        continuity_metrics.world_owner_restore_fallback_total);
    append_counter(
        "chat_continuity_world_migration_restore_total",
        continuity_metrics.world_migration_restore_total);
    append_counter(
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
    append_counter(
        "chat_continuity_world_migration_payload_room_handoff_total",
        continuity_metrics.world_migration_payload_room_handoff_total);
    append_counter(
        "chat_continuity_world_migration_payload_room_handoff_fallback_total",
        continuity_metrics.world_migration_payload_room_handoff_fallback_total);

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

    // dispatch 지연을 histogram으로 남겨야 p95/p99처럼 tail latency를 안정적으로 계산할 수 있다.
    // last/max 평균만 보면 짧은 spike와 지속적 악화를 구분하기 어렵다.
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
    append_gauge("chat_runtime_watchdog_total", static_cast<long double>(snap.watchdog_total));
    append_gauge("chat_runtime_watchdog_unhealthy_total", static_cast<long double>(snap.watchdog_unhealthy_total));
    append_counter("chat_runtime_watchdog_transition_total", snap.watchdog_transition_total);
    append_counter("chat_runtime_watchdog_freeze_suspect_total", snap.watchdog_freeze_suspect_total);
    append_counter("chat_runtime_detailed_telemetry_activation_total", snap.detailed_telemetry_activation_total);
    append_gauge("chat_runtime_detailed_telemetry_active", snap.detailed_telemetry_active ? 1.0L : 0.0L);
    append_gauge(
        "chat_runtime_detailed_telemetry_capture_budget_remaining",
        static_cast<long double>(snap.detailed_telemetry_capture_budget_remaining));
    append_counter(
        "chat_runtime_detailed_telemetry_captured_exception_total",
        snap.detailed_telemetry_captured_exception_total);
    append_gauge(
        "chat_runtime_detailed_telemetry_captured_dispatch_latency_max_ms",
        static_cast<long double>(snap.detailed_telemetry_captured_dispatch_latency_max_ns) / 1'000'000.0L);

    if (!snap.opcode_counts.empty()) {
        stream << "# TYPE chat_dispatch_opcode_total counter\n";
        for (const auto& [opcode, count] : snap.opcode_counts) {
            std::ostringstream label;
            label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
            stream << "chat_dispatch_opcode_total{opcode=\"0x" << label.str() << "\"} " << count << "\n";
        }

        // 같은 카운터를 사람이 읽기 쉬운 opcode 이름으로도 함께 노출한다.
        // 기존 숫자 라벨 메트릭은 유지해 두어야 이미 배포된 대시보드와 알람이 깨지지 않는다.
        // 즉, readability 개선은 compatibility를 깨지 않는 범위에서만 추가된다.
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

    // 채팅 훅 플러그인 상태를 별도 묶음으로 노출한다.
    // reload/순서/에러를 분리해 봐야 "플러그인이 느린가"와 "플러그인이 로드조차 안 됐는가"를 구분할 수 있다.
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

        server::app::chat::ChatHookPluginsMetrics pm;
        bool have_pm = false;
        if (const auto hook_plugin_metrics = server_chat_hook_plugin_metrics()) {
            pm = *hook_plugin_metrics;
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
            stream << "# TYPE plugin_reload_attempt_total counter\n";
            stream << "# TYPE plugin_reload_success_total counter\n";
            stream << "# TYPE plugin_reload_failure_total counter\n";
            stream << "# TYPE plugin_hook_calls_total counter\n";
            stream << "# TYPE plugin_hook_errors_total counter\n";
            stream << "# TYPE plugin_hook_duration_seconds histogram\n";

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

                std::string plugin_name = p.name;
                if (plugin_name.empty()) {
                    plugin_name = p.file;
                }
                const std::string plugin_labels =
                    std::string("plugin_name=\"") + escape_label_value(plugin_name) + "\"";
                append_counter_labeled("plugin_reload_attempt_total", plugin_labels, p.reload_attempt_total);
                append_counter_labeled("plugin_reload_success_total", plugin_labels, p.reload_success_total);
                append_counter_labeled("plugin_reload_failure_total", plugin_labels, p.reload_failure_total);

                for (const auto& hook_metric : p.hook_metrics) {
                    std::string hook_labels = std::string("hook_name=\"")
                                              + escape_label_value(hook_metric.hook_name)
                                              + "\",plugin_name=\""
                                              + escape_label_value(plugin_name)
                                              + "\"";
                    append_counter_labeled("plugin_hook_calls_total", hook_labels, hook_metric.calls_total);
                    append_counter_labeled("plugin_hook_errors_total", hook_labels, hook_metric.errors_total);

                    std::uint64_t bucket_cumulative = 0;
                    for (std::size_t bucket_index = 0;
                         bucket_index < kPluginHookDurationBucketUpperBoundsNs.size();
                         ++bucket_index) {
                        bucket_cumulative += hook_metric.duration_bucket_counts[bucket_index];
                        const long double le_seconds = static_cast<long double>(
                            kPluginHookDurationBucketUpperBoundsNs[bucket_index]) / 1'000'000'000.0L;

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

                if (p.loaded) {
                    std::string info_labels = labels;
                    info_labels += ",name=\"" + escape_label_value(p.name) + "\"";
                    info_labels += ",version=\"" + escape_label_value(p.version) + "\"";
                    append_gauge_labeled("chat_hook_plugin_info", info_labels, 1.0L);
                }
            }
        }
    }

    // Lua cold hook auto-disable 메트릭을 분리해, 스크립트가 단순 비활성인지
    // 연속 실패로 차단된 것인지를 운영에서 바로 확인할 수 있게 한다.
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

        server::app::chat::LuaHooksMetrics lua_metrics;
        bool have_lua_metrics = false;
        if (const auto lua_metrics_snapshot = server_chat_lua_hooks_metrics()) {
            lua_metrics = *lua_metrics_snapshot;
            have_lua_metrics = true;
        }

        stream << "chat_lua_hook_auto_disable_threshold "
               << static_cast<long double>(lua_metrics.auto_disable_threshold) << "\n";
        stream << "chat_lua_hooks_enabled " << (lua_metrics.enabled ? 1 : 0) << "\n";
        stream << "lua_loaded_scripts "
               << static_cast<long double>(lua_metrics.loaded_scripts) << "\n";
        stream << "lua_memory_used_bytes "
               << static_cast<long double>(lua_metrics.memory_used_bytes) << "\n";

        if (have_lua_metrics) {
            for (const auto& hook : lua_metrics.hooks) {
                const auto hook_name = escape_label_value(hook.hook_name);
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
                const auto hook_name = escape_label_value(script_call.hook_name);
                const auto script_name = escape_label_value(script_call.script_name);
                stream << "lua_script_calls_total{hook_name=\"" << hook_name
                       << "\",script_name=\"" << script_name << "\"} "
                       << script_call.calls_total << "\n";
                stream << "lua_script_errors_total{hook_name=\"" << hook_name
                       << "\",script_name=\"" << script_name << "\"} "
                       << script_call.errors_total << "\n";
            }
        }
    }

    stream << server_dependency_metrics_text();
    stream << server_lifecycle_metrics_text();
    if (const auto runtime_snapshot = server_runtime_snapshot()) {
        append_gauge(
            "chat_runtime_context_service_count",
            static_cast<long double>(runtime_snapshot->context_service_count));
        append_gauge(
            "chat_runtime_compatibility_bridge_count",
            static_cast<long double>(runtime_snapshot->compatibility_bridge_count));
        append_gauge(
            "chat_runtime_registered_module_count",
            static_cast<long double>(runtime_snapshot->registered_module_count));
        append_gauge(
            "chat_runtime_started_module_count",
            static_cast<long double>(runtime_snapshot->started_module_count));
        append_gauge(
            "chat_runtime_module_watchdog_count",
            static_cast<long double>(runtime_snapshot->watchdog_count));
        append_gauge(
            "chat_runtime_module_unhealthy_watchdog_count",
            static_cast<long double>(runtime_snapshot->unhealthy_watchdog_count));

        const auto modules = server_runtime_module_snapshot();
        if (!modules.empty()) {
            stream << "# TYPE chat_runtime_module_started gauge\n";
            stream << "# TYPE chat_runtime_module_watchdog_healthy gauge\n";
            for (const auto& module : modules) {
                const std::string labels =
                    std::string("module=\"") + escape_label_value(module.name) + "\"";
                append_labeled_gauge(
                    "chat_runtime_module_started",
                    labels,
                    module.started ? 1.0L : 0.0L);
                append_labeled_gauge(
                    "chat_runtime_module_watchdog_healthy",
                    labels,
                    (!module.has_watchdog || module.watchdog_healthy) ? 1.0L : 0.0L);
            }
        }
    }

    stream << std::setfill(' ') << std::dec << std::nouppercase;
    return stream.str();
}

} // namespace

std::string render_metrics_text() {
    return render_metrics_impl();
}

std::string render_logs_text() {
    return render_logs_impl();
}

void start_server_admin_http(server::core::app::EngineRuntime& runtime, unsigned short port) {
    runtime.start_admin_http(
        port,
        []() { return render_metrics_text(); },
        []() { return render_logs_text(); });
}

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
        []() { return render_metrics_text(); },
        []() { return server_health_ok(); },
        []() { return server_ready_ok(); },
        []() { return render_logs_text(); },
        [](bool ok) { return server_health_body(ok); },
        [](bool ok) { return server_readiness_body(ok); });
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
