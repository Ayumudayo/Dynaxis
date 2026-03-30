#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include "gateway/auth/authenticator.hpp"
#include "gateway/gateway_app.hpp"
#include "gateway/rudp_rollout_policy.hpp"
#include "gateway/session_directory.hpp"
#include "gateway/resilience_controls.hpp"
#include "gateway/udp_bind_abuse_guard.hpp"
#include "gateway/udp_sequenced_metrics.hpp"
#include "server/core/app/engine_runtime.hpp"
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"
#include "server/core/net/rudp/rudp_engine.hpp"
#include "server/core/storage/redis/client.hpp"

namespace gateway {

struct GatewayApp::Impl {
    struct SessionState {
        TransportSessionPtr session;
        std::string client_id;
        std::string backend_instance_id;
        bool udp_bound{false};
        boost::asio::ip::udp::endpoint udp_endpoint;
        std::uint64_t udp_nonce{0};
        std::uint64_t udp_expires_unix_ms{0};
        std::uint64_t udp_ticket_issued_unix_ms{0};
        std::uint32_t udp_bind_fail_attempts{0};
        std::uint64_t udp_bind_retry_after_unix_ms{0};
        std::string udp_token;
        UdpSequencedMetrics udp_sequenced_metrics;
        bool rudp_selected{false};
        bool rudp_fallback_to_tcp{false};
        std::unique_ptr<server::core::net::rudp::RudpEngine> rudp_engine;
    };

    Impl();
    ~Impl();

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    std::shared_ptr<server::core::net::TransportListener> listener_;
    server::core::app::EngineRuntime engine_;
    server::core::app::AppHost& app_host_;
    std::shared_ptr<auth::IAuthenticator> authenticator_;
    std::string gateway_id_;
    std::string listen_host_;
    std::uint16_t listen_port_{6000};
    bool allow_anonymous_{true};

    std::mutex session_mutex_;
    std::unordered_map<std::string, std::unique_ptr<SessionState>> sessions_;

    std::atomic<std::uint64_t> connections_total_{0};
    std::atomic<std::uint64_t> backend_resolve_fail_total_{0};
    std::atomic<std::uint64_t> backend_connect_fail_total_{0};
    std::atomic<std::uint64_t> backend_connect_timeout_total_{0};
    std::atomic<std::uint64_t> backend_write_error_total_{0};
    std::atomic<std::uint64_t> backend_send_queue_overflow_total_{0};
    std::atomic<std::uint64_t> backend_circuit_open_total_{0};
    std::atomic<std::uint64_t> backend_circuit_reject_total_{0};
    std::atomic<std::uint64_t> backend_connect_retry_total_{0};
    std::atomic<std::uint64_t> backend_retry_budget_exhausted_total_{0};
    std::atomic<std::uint64_t> resume_routing_bind_total_{0};
    std::atomic<std::uint64_t> resume_routing_hit_total_{0};
    std::atomic<std::uint64_t> resume_locator_bind_total_{0};
    std::atomic<std::uint64_t> resume_locator_lookup_hit_total_{0};
    std::atomic<std::uint64_t> resume_locator_lookup_miss_total_{0};
    std::atomic<std::uint64_t> resume_locator_selector_hit_total_{0};
    std::atomic<std::uint64_t> resume_locator_selector_fallback_total_{0};
    std::atomic<std::uint64_t> world_policy_filtered_sticky_total_{0};
    std::atomic<std::uint64_t> world_policy_filtered_candidate_total_{0};
    std::atomic<std::uint64_t> world_policy_replacement_selected_total_{0};

    std::atomic<std::uint64_t> ingress_reject_not_ready_total_{0};
    std::atomic<std::uint64_t> ingress_reject_rate_limit_total_{0};
    std::atomic<std::uint64_t> ingress_reject_session_limit_total_{0};
    std::atomic<std::uint64_t> ingress_reject_circuit_open_total_{0};

    std::string boot_id_;
    std::uint16_t metrics_port_{6001};
    std::uint32_t backend_connect_timeout_ms_{5000};
    std::size_t backend_send_queue_max_bytes_{256 * 1024};
    std::uint32_t backend_connect_retry_budget_per_min_{120};
    std::uint32_t backend_connect_retry_backoff_ms_{200};
    std::uint32_t backend_connect_retry_backoff_max_ms_{2000};
    bool backend_circuit_breaker_enabled_{true};
    std::uint32_t backend_circuit_fail_threshold_{5};
    std::uint32_t backend_circuit_open_ms_{10000};

    std::uint32_t ingress_tokens_per_sec_{200};
    std::uint32_t ingress_burst_tokens_{400};
    std::size_t ingress_max_active_sessions_{50000};

    gateway::TokenBucket ingress_token_bucket_{};
    gateway::RetryBudget backend_retry_budget_{};
    gateway::CircuitBreaker backend_circuit_breaker_{};

    std::string udp_listen_host_;
    std::uint16_t udp_listen_port_{0};
    std::string udp_bind_secret_;
    std::uint32_t udp_bind_ttl_ms_{5000};
    std::uint32_t udp_bind_fail_window_ms_{10000};
    std::uint32_t udp_bind_fail_limit_{5};
    std::uint32_t udp_bind_block_ms_{60000};
    std::uint32_t udp_bind_retry_backoff_ms_{200};
    std::uint32_t udp_bind_retry_backoff_max_ms_{2000};
    std::uint32_t udp_bind_retry_max_attempts_{6};
    std::unordered_set<std::uint16_t> udp_opcode_allowlist_{};
    RudpRolloutPolicy rudp_rollout_policy_{};
    std::unique_ptr<server::core::net::rudp::RudpConfig> rudp_config_;
    UdpBindAbuseGuard udp_bind_abuse_guard_;
    std::unique_ptr<boost::asio::ip::udp::socket> udp_socket_;
    boost::asio::ip::udp::endpoint udp_remote_endpoint_;
    std::array<std::uint8_t, 2048> udp_read_buffer_{};

    std::shared_ptr<server::core::storage::redis::IRedisClient> redis_client_;
    std::shared_ptr<server::core::discovery::IInstanceStateBackend> backend_registry_;
    std::unique_ptr<SessionDirectory> session_directory_;
    std::string redis_uri_;
    std::string continuity_prefix_;
    std::string session_directory_prefix_{"gateway/session/"};
    std::string resume_locator_prefix_{"gateway/session/locator/"};
    std::uint32_t resume_locator_ttl_sec_{900};

    std::atomic<bool> infra_probe_stop_{false};
    std::thread infra_probe_thread_;
    std::atomic<std::uint64_t> udp_packets_total_{0};
    std::atomic<std::uint64_t> udp_receive_error_total_{0};
    std::atomic<std::uint64_t> udp_send_error_total_{0};
    std::atomic<std::uint64_t> udp_bind_ticket_issued_total_{0};
    std::atomic<std::uint64_t> udp_bind_success_total_{0};
    std::atomic<std::uint64_t> udp_bind_reject_total_{0};
    std::atomic<std::uint64_t> udp_bind_block_total_{0};
    std::atomic<std::uint64_t> udp_bind_rate_limit_reject_total_{0};
    std::atomic<std::uint64_t> udp_bind_retry_backoff_total_{0};
    std::atomic<std::uint64_t> udp_bind_retry_reject_total_{0};
    std::atomic<std::uint64_t> udp_opcode_allowlist_reject_total_{0};
    std::atomic<std::uint64_t> udp_forward_total_{0};
    std::atomic<std::uint64_t> udp_forward_reliable_ordered_total_{0};
    std::atomic<std::uint64_t> udp_forward_reliable_total_{0};
    std::atomic<std::uint64_t> udp_forward_unreliable_sequenced_total_{0};
    std::atomic<std::uint64_t> udp_direct_state_delta_total_{0};
    std::atomic<std::uint64_t> udp_replay_drop_total_{0};
    std::atomic<std::uint64_t> udp_reorder_drop_total_{0};
    std::atomic<std::uint64_t> udp_duplicate_drop_total_{0};
    std::atomic<std::uint64_t> udp_retransmit_total_{0};
    std::atomic<std::uint64_t> udp_loss_estimated_total_{0};
    std::atomic<std::uint64_t> udp_jitter_ms_last_{0};
    std::atomic<std::uint64_t> udp_rtt_ms_last_{0};
    std::atomic<std::uint64_t> rudp_packets_total_{0};
    std::atomic<std::uint64_t> rudp_packets_reject_total_{0};
    std::atomic<std::uint64_t> rudp_inner_forward_total_{0};
    std::atomic<std::uint64_t> rudp_fallback_total_{0};
    std::atomic<std::uint64_t> rudp_direct_state_delta_total_{0};
    std::atomic<std::uint64_t> direct_state_delta_tcp_fallback_total_{0};
    std::atomic<bool> udp_enabled_{false};
};

} // namespace gateway
