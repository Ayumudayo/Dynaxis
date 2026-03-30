/**
 * @file gateway_metrics_text.cpp
 * @brief GatewayApp의 Prometheus metrics text rendering TU입니다.
 */
#include "gateway/gateway_app.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>

#include "gateway_app_access.hpp"
#include "gateway_app_state.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/metrics.hpp"

namespace gateway {

namespace {

constexpr bool kGatewayUdpIngressBuildEnabled = true;
constexpr bool kCoreRudpBuildEnabled = true;

std::uint64_t steady_time_ms() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(now.count());
}

} // namespace

std::string GatewayAppAccess::render_metrics_text(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    std::ostringstream stream;

    // build metadata(git hash/describe + build time)를 함께 노출해,
    // 운영자가 현재 어떤 바이너리가 떠 있는지 메트릭만으로도 확인할 수 있게 한다.
    server::core::metrics::append_build_info(stream);
    server::core::metrics::append_runtime_core_metrics(stream);
    server::core::metrics::append_prometheus_metrics(stream);

    stream << "# TYPE gateway_sessions_active gauge\n";
    {
        std::lock_guard<std::mutex> lock(impl_->session_mutex_);
        stream << "gateway_sessions_active " << impl_->sessions_.size() << "\n";
    }
    stream << "# TYPE gateway_connections_total counter\n";
    stream << "gateway_connections_total " << impl_->connections_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_resolve_fail_total counter\n";
    stream << "gateway_backend_resolve_fail_total "
           << impl_->backend_resolve_fail_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_connect_fail_total counter\n";
    stream << "gateway_backend_connect_fail_total "
           << impl_->backend_connect_fail_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_connect_timeout_total counter\n";
    stream << "gateway_backend_connect_timeout_total "
           << impl_->backend_connect_timeout_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_write_error_total counter\n";
    stream << "gateway_backend_write_error_total "
           << impl_->backend_write_error_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_send_queue_overflow_total counter\n";
    stream << "gateway_backend_send_queue_overflow_total "
           << impl_->backend_send_queue_overflow_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_connect_timeout_ms gauge\n";
    stream << "gateway_backend_connect_timeout_ms " << impl_->backend_connect_timeout_ms_ << "\n";

    stream << "# TYPE gateway_backend_send_queue_max_bytes gauge\n";
    stream << "gateway_backend_send_queue_max_bytes " << impl_->backend_send_queue_max_bytes_ << "\n";

    stream << "# TYPE gateway_backend_circuit_open_total counter\n";
    stream << "gateway_backend_circuit_open_total "
           << impl_->backend_circuit_open_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_circuit_reject_total counter\n";
    stream << "gateway_backend_circuit_reject_total "
           << impl_->backend_circuit_reject_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_connect_retry_total counter\n";
    stream << "gateway_backend_connect_retry_total "
           << impl_->backend_connect_retry_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_backend_retry_budget_exhausted_total counter\n";
    stream << "gateway_backend_retry_budget_exhausted_total "
           << impl_->backend_retry_budget_exhausted_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_routing_bind_total counter\n";
    stream << "gateway_resume_routing_bind_total "
           << impl_->resume_routing_bind_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_routing_hit_total counter\n";
    stream << "gateway_resume_routing_hit_total "
           << impl_->resume_routing_hit_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_locator_bind_total counter\n";
    stream << "gateway_resume_locator_bind_total "
           << impl_->resume_locator_bind_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_locator_lookup_hit_total counter\n";
    stream << "gateway_resume_locator_lookup_hit_total "
           << impl_->resume_locator_lookup_hit_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_locator_lookup_miss_total counter\n";
    stream << "gateway_resume_locator_lookup_miss_total "
           << impl_->resume_locator_lookup_miss_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_locator_selector_hit_total counter\n";
    stream << "gateway_resume_locator_selector_hit_total "
           << impl_->resume_locator_selector_hit_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_locator_selector_fallback_total counter\n";
    stream << "gateway_resume_locator_selector_fallback_total "
           << impl_->resume_locator_selector_fallback_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_resume_locator_ttl_sec gauge\n";
    stream << "gateway_resume_locator_ttl_sec "
           << impl_->resume_locator_ttl_sec_ << "\n";

    stream << "# TYPE gateway_backend_circuit_open gauge\n";
    stream << "gateway_backend_circuit_open "
           << (impl_->backend_circuit_breaker_.is_open(steady_time_ms()) ? 1 : 0) << "\n";

    stream << "# TYPE gateway_backend_circuit_fail_threshold gauge\n";
    stream << "gateway_backend_circuit_fail_threshold " << impl_->backend_circuit_fail_threshold_ << "\n";

    stream << "# TYPE gateway_backend_circuit_open_ms gauge\n";
    stream << "gateway_backend_circuit_open_ms " << impl_->backend_circuit_open_ms_ << "\n";

    stream << "# TYPE gateway_backend_connect_retry_budget_per_min gauge\n";
    stream << "gateway_backend_connect_retry_budget_per_min " << impl_->backend_connect_retry_budget_per_min_ << "\n";

    stream << "# TYPE gateway_backend_connect_retry_backoff_ms gauge\n";
    stream << "gateway_backend_connect_retry_backoff_ms " << impl_->backend_connect_retry_backoff_ms_ << "\n";

    stream << "# TYPE gateway_backend_connect_retry_backoff_max_ms gauge\n";
    stream << "gateway_backend_connect_retry_backoff_max_ms " << impl_->backend_connect_retry_backoff_max_ms_ << "\n";

    stream << "# TYPE gateway_ingress_reject_not_ready_total counter\n";
    stream << "gateway_ingress_reject_not_ready_total "
           << impl_->ingress_reject_not_ready_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_ingress_reject_rate_limit_total counter\n";
    stream << "gateway_ingress_reject_rate_limit_total "
           << impl_->ingress_reject_rate_limit_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_ingress_reject_session_limit_total counter\n";
    stream << "gateway_ingress_reject_session_limit_total "
           << impl_->ingress_reject_session_limit_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_ingress_reject_circuit_open_total counter\n";
    stream << "gateway_ingress_reject_circuit_open_total "
           << impl_->ingress_reject_circuit_open_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_ingress_tokens_per_sec gauge\n";
    stream << "gateway_ingress_tokens_per_sec " << impl_->ingress_tokens_per_sec_ << "\n";

    stream << "# TYPE gateway_ingress_burst_tokens gauge\n";
    stream << "gateway_ingress_burst_tokens " << impl_->ingress_burst_tokens_ << "\n";

    stream << "# TYPE gateway_ingress_max_active_sessions gauge\n";
    stream << "gateway_ingress_max_active_sessions " << impl_->ingress_max_active_sessions_ << "\n";

    stream << "# TYPE gateway_ingress_tokens_available gauge\n";
    stream << "gateway_ingress_tokens_available " << impl_->ingress_token_bucket_.available(steady_time_ms()) << "\n";

    stream << "# TYPE gateway_udp_enabled gauge\n";
    stream << "gateway_udp_enabled " << (impl_->udp_enabled_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

    stream << "# TYPE gateway_udp_ingress_feature_enabled gauge\n";
    stream << "gateway_udp_ingress_feature_enabled " << (kGatewayUdpIngressBuildEnabled ? 1 : 0) << "\n";

    stream << "# TYPE gateway_rudp_core_build_enabled gauge\n";
    stream << "gateway_rudp_core_build_enabled " << (kCoreRudpBuildEnabled ? 1 : 0) << "\n";

    stream << "# TYPE gateway_rudp_enabled gauge\n";
    stream << "gateway_rudp_enabled " << (impl_->rudp_rollout_policy_.enabled ? 1 : 0) << "\n";

    stream << "# TYPE gateway_rudp_canary_percent gauge\n";
    stream << "gateway_rudp_canary_percent " << impl_->rudp_rollout_policy_.canary_percent << "\n";

    stream << "# TYPE gateway_rudp_opcode_allowlist_size gauge\n";
    stream << "gateway_rudp_opcode_allowlist_size " << impl_->rudp_rollout_policy_.opcode_allowlist.size() << "\n";

    stream << "# TYPE gateway_udp_packets_total counter\n";
    stream << "gateway_udp_packets_total " << impl_->udp_packets_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_receive_error_total counter\n";
    stream << "gateway_udp_receive_error_total " << impl_->udp_receive_error_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_send_error_total counter\n";
    stream << "gateway_udp_send_error_total " << impl_->udp_send_error_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_ticket_issued_total counter\n";
    stream << "gateway_udp_bind_ticket_issued_total "
           << impl_->udp_bind_ticket_issued_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_success_total counter\n";
    stream << "gateway_udp_bind_success_total "
           << impl_->udp_bind_success_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_reject_total counter\n";
    stream << "gateway_udp_bind_reject_total "
           << impl_->udp_bind_reject_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_block_total counter\n";
    stream << "gateway_udp_bind_block_total "
           << impl_->udp_bind_block_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_rate_limit_reject_total counter\n";
    stream << "gateway_udp_bind_rate_limit_reject_total "
           << impl_->udp_bind_rate_limit_reject_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_retry_backoff_total counter\n";
    stream << "gateway_udp_bind_retry_backoff_total "
           << impl_->udp_bind_retry_backoff_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_retry_reject_total counter\n";
    stream << "gateway_udp_bind_retry_reject_total "
           << impl_->udp_bind_retry_reject_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_opcode_allowlist_reject_total counter\n";
    stream << "gateway_udp_opcode_allowlist_reject_total "
           << impl_->udp_opcode_allowlist_reject_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_bind_fail_window_ms gauge\n";
    stream << "gateway_udp_bind_fail_window_ms " << impl_->udp_bind_fail_window_ms_ << "\n";

    stream << "# TYPE gateway_udp_bind_fail_limit gauge\n";
    stream << "gateway_udp_bind_fail_limit " << impl_->udp_bind_fail_limit_ << "\n";

    stream << "# TYPE gateway_udp_bind_block_ms gauge\n";
    stream << "gateway_udp_bind_block_ms " << impl_->udp_bind_block_ms_ << "\n";

    stream << "# TYPE gateway_udp_bind_retry_backoff_ms gauge\n";
    stream << "gateway_udp_bind_retry_backoff_ms " << impl_->udp_bind_retry_backoff_ms_ << "\n";

    stream << "# TYPE gateway_udp_bind_retry_backoff_max_ms gauge\n";
    stream << "gateway_udp_bind_retry_backoff_max_ms " << impl_->udp_bind_retry_backoff_max_ms_ << "\n";

    stream << "# TYPE gateway_udp_bind_retry_max_attempts gauge\n";
    stream << "gateway_udp_bind_retry_max_attempts " << impl_->udp_bind_retry_max_attempts_ << "\n";

    stream << "# TYPE gateway_udp_opcode_allowlist_size gauge\n";
    stream << "gateway_udp_opcode_allowlist_size " << impl_->udp_opcode_allowlist_.size() << "\n";

    stream << "# TYPE gateway_udp_bind_ttl_ms gauge\n";
    stream << "gateway_udp_bind_ttl_ms " << impl_->udp_bind_ttl_ms_ << "\n";

    stream << "# TYPE gateway_udp_forward_total counter\n";
    stream << "gateway_udp_forward_total "
           << impl_->udp_forward_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_transport_delivery_forward_total counter\n";
    stream << "gateway_transport_delivery_forward_total{transport=\"udp\",delivery=\"reliable_ordered\"} "
           << impl_->udp_forward_reliable_ordered_total_.load(std::memory_order_relaxed) << "\n";
    stream << "gateway_transport_delivery_forward_total{transport=\"udp\",delivery=\"reliable\"} "
           << impl_->udp_forward_reliable_total_.load(std::memory_order_relaxed) << "\n";
    stream << "gateway_transport_delivery_forward_total{transport=\"udp\",delivery=\"unreliable_sequenced\"} "
           << impl_->udp_forward_unreliable_sequenced_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_udp_direct_state_delta_total counter\n";
    stream << "gateway_udp_direct_state_delta_total "
           << impl_->udp_direct_state_delta_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_replay_drop_total counter\n";
    stream << "gateway_udp_replay_drop_total "
           << impl_->udp_replay_drop_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_reorder_drop_total counter\n";
    stream << "gateway_udp_reorder_drop_total "
           << impl_->udp_reorder_drop_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_duplicate_drop_total counter\n";
    stream << "gateway_udp_duplicate_drop_total "
           << impl_->udp_duplicate_drop_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_transport_delivery_drop_total counter\n";
    stream << "gateway_transport_delivery_drop_total{transport=\"udp\",delivery=\"unreliable_sequenced\",reason=\"replay\"} "
           << impl_->udp_replay_drop_total_.load(std::memory_order_relaxed) << "\n";
    stream << "gateway_transport_delivery_drop_total{transport=\"udp\",delivery=\"unreliable_sequenced\",reason=\"reorder\"} "
           << impl_->udp_reorder_drop_total_.load(std::memory_order_relaxed) << "\n";
    stream << "gateway_transport_delivery_drop_total{transport=\"udp\",delivery=\"unreliable_sequenced\",reason=\"duplicate\"} "
           << impl_->udp_duplicate_drop_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_retransmit_total counter\n";
    stream << "gateway_udp_retransmit_total "
           << impl_->udp_retransmit_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_loss_estimated_total counter\n";
    stream << "gateway_udp_loss_estimated_total "
           << impl_->udp_loss_estimated_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_jitter_ms_last gauge\n";
    stream << "gateway_udp_jitter_ms_last "
           << impl_->udp_jitter_ms_last_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_udp_rtt_ms_last gauge\n";
    stream << "gateway_udp_rtt_ms_last "
           << impl_->udp_rtt_ms_last_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_rudp_packets_total counter\n";
    stream << "gateway_rudp_packets_total "
           << impl_->rudp_packets_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_rudp_packets_reject_total counter\n";
    stream << "gateway_rudp_packets_reject_total "
           << impl_->rudp_packets_reject_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_rudp_inner_forward_total counter\n";
    stream << "gateway_rudp_inner_forward_total "
           << impl_->rudp_inner_forward_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_rudp_fallback_total counter\n";
    stream << "gateway_rudp_fallback_total "
           << impl_->rudp_fallback_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_rudp_direct_state_delta_total counter\n";
    stream << "gateway_rudp_direct_state_delta_total "
           << impl_->rudp_direct_state_delta_total_.load(std::memory_order_relaxed) << "\n";
    stream << "# TYPE gateway_direct_state_delta_tcp_fallback_total counter\n";
    stream << "gateway_direct_state_delta_tcp_fallback_total "
           << impl_->direct_state_delta_tcp_fallback_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_world_policy_filtered_total counter\n";
    stream << "gateway_world_policy_filtered_total{source=\"sticky\"} "
           << impl_->world_policy_filtered_sticky_total_.load(std::memory_order_relaxed) << "\n";
    stream << "gateway_world_policy_filtered_total{source=\"candidate\"} "
           << impl_->world_policy_filtered_candidate_total_.load(std::memory_order_relaxed) << "\n";

    stream << "# TYPE gateway_world_policy_replacement_selected_total counter\n";
    stream << "gateway_world_policy_replacement_selected_total "
           << impl_->world_policy_replacement_selected_total_.load(std::memory_order_relaxed) << "\n";

    stream << impl_->app_host_.dependency_metrics_text();
    stream << impl_->app_host_.lifecycle_metrics_text();
    return stream.str();
}

} // namespace gateway
