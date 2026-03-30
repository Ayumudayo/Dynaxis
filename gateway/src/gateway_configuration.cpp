/**
 * @file gateway_configuration.cpp
 * @brief GatewayApp 설정 파싱과 인프라 초기화 구현입니다.
 */
#include "gateway/gateway_app.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "gateway/registry_backend_factory.hpp"
#include "gateway/rudp_rollout_policy.hpp"
#include "gateway/session_directory.hpp"
#include "gateway_app_access.hpp"
#include "gateway_app_state.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/util/log.hpp"
#include "server/protocol/game_opcodes.hpp"

namespace gateway {

namespace {

constexpr const char* kEnvGatewayListen = "GATEWAY_LISTEN";
constexpr const char* kEnvGatewayId = "GATEWAY_ID";
constexpr const char* kEnvRedisUri = "REDIS_URI";
constexpr const char* kEnvServerRegistryPrefix = "SERVER_REGISTRY_PREFIX";
constexpr const char* kEnvServerRegistryTtl = "SERVER_REGISTRY_TTL";
constexpr const char* kEnvRedisChannelPrefix = "REDIS_CHANNEL_PREFIX";
constexpr const char* kEnvSessionContinuityRedisPrefix = "SESSION_CONTINUITY_REDIS_PREFIX";
constexpr const char* kEnvGatewayBackendConnectTimeoutMs = "GATEWAY_BACKEND_CONNECT_TIMEOUT_MS";
constexpr const char* kEnvGatewayBackendSendQueueMaxBytes = "GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES";
constexpr const char* kEnvGatewayBackendCircuitEnabled = "GATEWAY_BACKEND_CIRCUIT_BREAKER_ENABLED";
constexpr const char* kEnvGatewayBackendCircuitFailThreshold = "GATEWAY_BACKEND_CIRCUIT_FAIL_THRESHOLD";
constexpr const char* kEnvGatewayBackendCircuitOpenMs = "GATEWAY_BACKEND_CIRCUIT_OPEN_MS";
constexpr const char* kEnvGatewayBackendRetryBudgetPerMin = "GATEWAY_BACKEND_CONNECT_RETRY_BUDGET_PER_MIN";
constexpr const char* kEnvGatewayBackendRetryBackoffMs = "GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MS";
constexpr const char* kEnvGatewayBackendRetryBackoffMaxMs = "GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MAX_MS";
constexpr const char* kEnvGatewayIngressTokensPerSec = "GATEWAY_INGRESS_TOKENS_PER_SEC";
constexpr const char* kEnvGatewayIngressBurstTokens = "GATEWAY_INGRESS_BURST_TOKENS";
constexpr const char* kEnvGatewayIngressMaxActiveSessions = "GATEWAY_INGRESS_MAX_ACTIVE_SESSIONS";
constexpr const char* kEnvAllowAnonymous = "ALLOW_ANONYMOUS";
constexpr const char* kEnvGatewayUdpListen = "GATEWAY_UDP_LISTEN";
constexpr const char* kEnvGatewayUdpBindSecret = "GATEWAY_UDP_BIND_SECRET";
constexpr const char* kEnvGatewayUdpBindTtlMs = "GATEWAY_UDP_BIND_TTL_MS";
constexpr const char* kEnvGatewayUdpBindFailWindowMs = "GATEWAY_UDP_BIND_FAIL_WINDOW_MS";
constexpr const char* kEnvGatewayUdpBindFailLimit = "GATEWAY_UDP_BIND_FAIL_LIMIT";
constexpr const char* kEnvGatewayUdpBindBlockMs = "GATEWAY_UDP_BIND_BLOCK_MS";
constexpr const char* kEnvGatewayUdpBindRetryBackoffMs = "GATEWAY_UDP_BIND_RETRY_BACKOFF_MS";
constexpr const char* kEnvGatewayUdpBindRetryBackoffMaxMs = "GATEWAY_UDP_BIND_RETRY_BACKOFF_MAX_MS";
constexpr const char* kEnvGatewayUdpBindRetryMaxAttempts = "GATEWAY_UDP_BIND_RETRY_MAX_ATTEMPTS";
constexpr const char* kEnvGatewayUdpOpcodeAllowlist = "GATEWAY_UDP_OPCODE_ALLOWLIST";
constexpr const char* kEnvGatewayRudpEnable = "GATEWAY_RUDP_ENABLE";
constexpr const char* kEnvGatewayRudpCanaryPercent = "GATEWAY_RUDP_CANARY_PERCENT";
constexpr const char* kEnvGatewayRudpOpcodeAllowlist = "GATEWAY_RUDP_OPCODE_ALLOWLIST";
constexpr const char* kEnvGatewayRudpHandshakeTimeoutMs = "GATEWAY_RUDP_HANDSHAKE_TIMEOUT_MS";
constexpr const char* kEnvGatewayRudpIdleTimeoutMs = "GATEWAY_RUDP_IDLE_TIMEOUT_MS";
constexpr const char* kEnvGatewayRudpAckDelayMs = "GATEWAY_RUDP_ACK_DELAY_MS";
constexpr const char* kEnvGatewayRudpMaxInflightPackets = "GATEWAY_RUDP_MAX_INFLIGHT_PACKETS";
constexpr const char* kEnvGatewayRudpMaxInflightBytes = "GATEWAY_RUDP_MAX_INFLIGHT_BYTES";
constexpr const char* kEnvGatewayRudpMtuPayloadBytes = "GATEWAY_RUDP_MTU_PAYLOAD_BYTES";
constexpr const char* kEnvGatewayRudpRtoMinMs = "GATEWAY_RUDP_RTO_MIN_MS";
constexpr const char* kEnvGatewayRudpRtoMaxMs = "GATEWAY_RUDP_RTO_MAX_MS";
constexpr const char* kEnvSessionContinuityLeaseTtlSec = "SESSION_CONTINUITY_LEASE_TTL_SEC";
constexpr const char* kDefaultGatewayListen = "0.0.0.0:6000";
constexpr const char* kDefaultGatewayId = "gateway-default";
constexpr const char* kDefaultRedisUri = "tcp://127.0.0.1:6379";
constexpr const char* kDefaultServerRegistryPrefix = "gateway/instances/";
constexpr const char* kDefaultSessionDirectoryPrefix = "gateway/session/";
constexpr std::uint32_t kDefaultBackendConnectTimeoutMs = 5000;
constexpr std::size_t kDefaultBackendSendQueueMaxBytes = 256 * 1024;
constexpr bool kDefaultBackendCircuitEnabled = true;
constexpr std::uint32_t kDefaultBackendCircuitFailThreshold = 5;
constexpr std::uint32_t kDefaultBackendCircuitOpenMs = 10000;
constexpr std::uint32_t kDefaultBackendRetryBudgetPerMin = 120;
constexpr std::uint32_t kDefaultBackendRetryBackoffMs = 200;
constexpr std::uint32_t kDefaultBackendRetryBackoffMaxMs = 2000;
constexpr std::uint32_t kDefaultIngressTokensPerSec = 200;
constexpr std::uint32_t kDefaultIngressBurstTokens = 400;
constexpr std::size_t kDefaultIngressMaxActiveSessions = 50000;
constexpr std::uint32_t kDefaultUdpBindTtlMs = 5000;
constexpr std::uint32_t kDefaultUdpBindFailWindowMs = 10000;
constexpr std::uint32_t kDefaultUdpBindFailLimit = 5;
constexpr std::uint32_t kDefaultUdpBindBlockMs = 60000;
constexpr std::uint32_t kDefaultUdpBindRetryBackoffMs = 200;
constexpr std::uint32_t kDefaultUdpBindRetryBackoffMaxMs = 2000;
constexpr std::uint32_t kDefaultUdpBindRetryMaxAttempts = 6;
constexpr bool kDefaultGatewayRudpEnable = false;
constexpr std::uint32_t kDefaultGatewayRudpCanaryPercent = 0;
constexpr std::uint32_t kDefaultResumeLocatorTtlSec = 900;
constexpr bool kGatewayUdpIngressBuildEnabled = true;
constexpr bool kCoreRudpBuildEnabled = true;

bool parse_env_bool(const char* key, bool fallback) {
    if (const char* value = std::getenv(key); value && *value) {
        const auto text = std::string_view(value);
        if (text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON") {
            return true;
        }
        if (text == "0" || text == "false" || text == "FALSE" || text == "off" || text == "OFF") {
            return false;
        }
    }
    return fallback;
}

template <typename T>
T parse_env_integral_bounded(const char* key,
                             T fallback,
                             T min_value,
                             T max_value,
                             const char* warning_message) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if (const char* value = std::getenv(key); value && *value) {
        try {
            const auto parsed = std::stoull(value);
            if (parsed >= min_value && parsed <= max_value) {
                return static_cast<T>(parsed);
            }
        } catch (...) {
        }
        server::core::log::warn(warning_message);
    }
    return fallback;
}

std::uint32_t parse_env_u32_bounded(const char* key,
                                    std::uint32_t fallback,
                                    std::uint32_t min_value,
                                    std::uint32_t max_value,
                                    const char* warning_message) {
    return parse_env_integral_bounded<std::uint32_t>(key, fallback, min_value, max_value, warning_message);
}

std::size_t parse_env_size_bounded(const char* key,
                                   std::size_t fallback,
                                   std::size_t min_value,
                                   std::size_t max_value,
                                   const char* warning_message) {
    return parse_env_integral_bounded<std::size_t>(key, fallback, min_value, max_value, warning_message);
}

std::pair<std::string, std::uint16_t> parse_listen(std::string_view value, std::uint16_t fallback_port) {
    if (value.empty()) {
        return {"0.0.0.0", fallback_port};
    }

    const auto delimiter = value.find(':');
    if (delimiter == std::string_view::npos) {
        return {std::string(value), fallback_port};
    }

    std::string host(value.substr(0, delimiter));
    const std::string_view port_view = value.substr(delimiter + 1);
    std::uint16_t port = fallback_port;
    if (!port_view.empty()) {
        try {
            port = static_cast<std::uint16_t>(std::stoul(std::string(port_view)));
        } catch (...) {
            server::core::log::warn("GatewayApp invalid port in GATEWAY_LISTEN; falling back to default");
            port = fallback_port;
        }
    }
    return {std::move(host), port};
}

} // namespace

void GatewayAppAccess::configure_gateway(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    const char* listen_env = std::getenv(kEnvGatewayListen);
    const auto [host, port] =
        parse_listen(listen_env ? std::string_view(listen_env) : std::string_view(kDefaultGatewayListen),
                     impl_->listen_port_);
    impl_->listen_host_ = host;
    impl_->listen_port_ = port;

    const char* id_env = std::getenv(kEnvGatewayId);
    if (id_env && *id_env) {
        impl_->gateway_id_ = id_env;
    } else {
        impl_->gateway_id_ = kDefaultGatewayId;
    }

    impl_->backend_connect_timeout_ms_ = parse_env_u32_bounded(
        kEnvGatewayBackendConnectTimeoutMs,
        kDefaultBackendConnectTimeoutMs,
        100,
        60000,
        "GatewayApp invalid GATEWAY_BACKEND_CONNECT_TIMEOUT_MS; using default");

    impl_->backend_send_queue_max_bytes_ = parse_env_size_bounded(
        kEnvGatewayBackendSendQueueMaxBytes,
        kDefaultBackendSendQueueMaxBytes,
        1024,
        16 * 1024 * 1024,
        "GatewayApp invalid GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES; using default");

    impl_->backend_circuit_breaker_enabled_ =
        parse_env_bool(kEnvGatewayBackendCircuitEnabled, kDefaultBackendCircuitEnabled);

    impl_->backend_circuit_fail_threshold_ = parse_env_u32_bounded(
        kEnvGatewayBackendCircuitFailThreshold,
        kDefaultBackendCircuitFailThreshold,
        1,
        100,
        "GatewayApp invalid GATEWAY_BACKEND_CIRCUIT_FAIL_THRESHOLD; using default");

    impl_->backend_circuit_open_ms_ = parse_env_u32_bounded(
        kEnvGatewayBackendCircuitOpenMs,
        kDefaultBackendCircuitOpenMs,
        100,
        300000,
        "GatewayApp invalid GATEWAY_BACKEND_CIRCUIT_OPEN_MS; using default");

    impl_->backend_connect_retry_budget_per_min_ = parse_env_u32_bounded(
        kEnvGatewayBackendRetryBudgetPerMin,
        kDefaultBackendRetryBudgetPerMin,
        0,
        60000,
        "GatewayApp invalid GATEWAY_BACKEND_CONNECT_RETRY_BUDGET_PER_MIN; using default");

    impl_->backend_connect_retry_backoff_ms_ = parse_env_u32_bounded(
        kEnvGatewayBackendRetryBackoffMs,
        kDefaultBackendRetryBackoffMs,
        10,
        60000,
        "GatewayApp invalid GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MS; using default");

    impl_->backend_connect_retry_backoff_max_ms_ = parse_env_u32_bounded(
        kEnvGatewayBackendRetryBackoffMaxMs,
        kDefaultBackendRetryBackoffMaxMs,
        10,
        300000,
        "GatewayApp invalid GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MAX_MS; using default");
    if (impl_->backend_connect_retry_backoff_max_ms_ < impl_->backend_connect_retry_backoff_ms_) {
        impl_->backend_connect_retry_backoff_max_ms_ = impl_->backend_connect_retry_backoff_ms_;
    }

    impl_->ingress_tokens_per_sec_ = parse_env_u32_bounded(
        kEnvGatewayIngressTokensPerSec,
        kDefaultIngressTokensPerSec,
        1,
        100000,
        "GatewayApp invalid GATEWAY_INGRESS_TOKENS_PER_SEC; using default");

    impl_->ingress_burst_tokens_ = parse_env_u32_bounded(
        kEnvGatewayIngressBurstTokens,
        kDefaultIngressBurstTokens,
        1,
        200000,
        "GatewayApp invalid GATEWAY_INGRESS_BURST_TOKENS; using default");
    if (impl_->ingress_burst_tokens_ < impl_->ingress_tokens_per_sec_) {
        impl_->ingress_burst_tokens_ = impl_->ingress_tokens_per_sec_;
    }

    impl_->ingress_max_active_sessions_ = parse_env_size_bounded(
        kEnvGatewayIngressMaxActiveSessions,
        kDefaultIngressMaxActiveSessions,
        1,
        500000,
        "GatewayApp invalid GATEWAY_INGRESS_MAX_ACTIVE_SESSIONS; using default");

    impl_->ingress_token_bucket_.configure(
        static_cast<double>(impl_->ingress_tokens_per_sec_),
        static_cast<double>(impl_->ingress_burst_tokens_));
    impl_->backend_retry_budget_.configure(impl_->backend_connect_retry_budget_per_min_, 60000);
    impl_->backend_circuit_breaker_.configure(
        impl_->backend_circuit_breaker_enabled_,
        impl_->backend_circuit_fail_threshold_,
        impl_->backend_circuit_open_ms_);

    if (const char* udp_listen_env = std::getenv(kEnvGatewayUdpListen); udp_listen_env && *udp_listen_env) {
        const auto [udp_host, udp_port] = parse_listen(std::string_view(udp_listen_env), 0);
        impl_->udp_listen_host_ = udp_host;
        impl_->udp_listen_port_ = udp_port;
    }

    if (const char* udp_secret_env = std::getenv(kEnvGatewayUdpBindSecret); udp_secret_env && *udp_secret_env) {
        impl_->udp_bind_secret_ = udp_secret_env;
    }

    impl_->udp_bind_ttl_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindTtlMs,
        kDefaultUdpBindTtlMs,
        1000,
        120000,
        "GatewayApp invalid GATEWAY_UDP_BIND_TTL_MS; using default");

    impl_->udp_bind_fail_window_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindFailWindowMs,
        kDefaultUdpBindFailWindowMs,
        1000,
        120000,
        "GatewayApp invalid GATEWAY_UDP_BIND_FAIL_WINDOW_MS; using default");

    impl_->udp_bind_fail_limit_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindFailLimit,
        kDefaultUdpBindFailLimit,
        2,
        100,
        "GatewayApp invalid GATEWAY_UDP_BIND_FAIL_LIMIT; using default");

    impl_->udp_bind_block_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindBlockMs,
        kDefaultUdpBindBlockMs,
        1000,
        300000,
        "GatewayApp invalid GATEWAY_UDP_BIND_BLOCK_MS; using default");

    impl_->udp_bind_retry_backoff_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindRetryBackoffMs,
        kDefaultUdpBindRetryBackoffMs,
        10,
        60000,
        "GatewayApp invalid GATEWAY_UDP_BIND_RETRY_BACKOFF_MS; using default");

    impl_->udp_bind_retry_backoff_max_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindRetryBackoffMaxMs,
        kDefaultUdpBindRetryBackoffMaxMs,
        10,
        300000,
        "GatewayApp invalid GATEWAY_UDP_BIND_RETRY_BACKOFF_MAX_MS; using default");
    if (impl_->udp_bind_retry_backoff_max_ms_ < impl_->udp_bind_retry_backoff_ms_) {
        impl_->udp_bind_retry_backoff_max_ms_ = impl_->udp_bind_retry_backoff_ms_;
    }

    impl_->udp_bind_retry_max_attempts_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindRetryMaxAttempts,
        kDefaultUdpBindRetryMaxAttempts,
        1,
        32,
        "GatewayApp invalid GATEWAY_UDP_BIND_RETRY_MAX_ATTEMPTS; using default");

    if (const char* udp_allowlist_env = std::getenv(kEnvGatewayUdpOpcodeAllowlist);
        udp_allowlist_env && *udp_allowlist_env) {
        impl_->udp_opcode_allowlist_ = gateway::parse_udp_opcode_allowlist(udp_allowlist_env);
    } else {
        impl_->udp_opcode_allowlist_.clear();
    }
    if (impl_->udp_opcode_allowlist_.empty()) {
        impl_->udp_opcode_allowlist_.insert(server::protocol::MSG_UDP_BIND_REQ);
    }

    impl_->rudp_rollout_policy_.enabled = parse_env_bool(kEnvGatewayRudpEnable, kDefaultGatewayRudpEnable);
    impl_->rudp_rollout_policy_.canary_percent = parse_env_u32_bounded(
        kEnvGatewayRudpCanaryPercent,
        kDefaultGatewayRudpCanaryPercent,
        0,
        100,
        "GatewayApp invalid GATEWAY_RUDP_CANARY_PERCENT; using default");

    if (const char* allowlist_env = std::getenv(kEnvGatewayRudpOpcodeAllowlist); allowlist_env && *allowlist_env) {
        impl_->rudp_rollout_policy_.opcode_allowlist = gateway::parse_rudp_opcode_allowlist(allowlist_env);
    } else {
        impl_->rudp_rollout_policy_.opcode_allowlist.clear();
    }

    impl_->rudp_config_->handshake_timeout_ms = parse_env_u32_bounded(
        kEnvGatewayRudpHandshakeTimeoutMs,
        1500,
        100,
        60000,
        "GatewayApp invalid GATEWAY_RUDP_HANDSHAKE_TIMEOUT_MS; using default");
    impl_->rudp_config_->idle_timeout_ms = parse_env_u32_bounded(
        kEnvGatewayRudpIdleTimeoutMs,
        10000,
        1000,
        300000,
        "GatewayApp invalid GATEWAY_RUDP_IDLE_TIMEOUT_MS; using default");
    impl_->rudp_config_->ack_delay_ms = parse_env_u32_bounded(
        kEnvGatewayRudpAckDelayMs,
        10,
        1,
        200,
        "GatewayApp invalid GATEWAY_RUDP_ACK_DELAY_MS; using default");
    impl_->rudp_config_->max_inflight_packets = parse_env_size_bounded(
        kEnvGatewayRudpMaxInflightPackets,
        256,
        1,
        4096,
        "GatewayApp invalid GATEWAY_RUDP_MAX_INFLIGHT_PACKETS; using default");
    impl_->rudp_config_->max_inflight_bytes = parse_env_size_bounded(
        kEnvGatewayRudpMaxInflightBytes,
        256 * 1024,
        1024,
        16 * 1024 * 1024,
        "GatewayApp invalid GATEWAY_RUDP_MAX_INFLIGHT_BYTES; using default");
    impl_->rudp_config_->mtu_payload_bytes = parse_env_size_bounded(
        kEnvGatewayRudpMtuPayloadBytes,
        1200,
        256,
        1400,
        "GatewayApp invalid GATEWAY_RUDP_MTU_PAYLOAD_BYTES; using default");
    impl_->rudp_config_->rto_min_ms = parse_env_u32_bounded(
        kEnvGatewayRudpRtoMinMs,
        50,
        1,
        10000,
        "GatewayApp invalid GATEWAY_RUDP_RTO_MIN_MS; using default");
    impl_->rudp_config_->rto_max_ms = parse_env_u32_bounded(
        kEnvGatewayRudpRtoMaxMs,
        2000,
        1,
        60000,
        "GatewayApp invalid GATEWAY_RUDP_RTO_MAX_MS; using default");
    if (impl_->rudp_config_->rto_max_ms < impl_->rudp_config_->rto_min_ms) {
        impl_->rudp_config_->rto_max_ms = impl_->rudp_config_->rto_min_ms;
    }

    if (!impl_->rudp_rollout_policy_.enabled) {
        impl_->rudp_rollout_policy_.canary_percent = 0;
    }

    impl_->udp_bind_abuse_guard_.configure(
        impl_->udp_bind_fail_window_ms_,
        impl_->udp_bind_fail_limit_,
        impl_->udp_bind_block_ms_);

    impl_->allow_anonymous_ = true;
    if (const char* anonymous_env = std::getenv(kEnvAllowAnonymous); anonymous_env && *anonymous_env) {
        impl_->allow_anonymous_ = (std::string_view(anonymous_env) != "0");
    }

    server::core::log::info(
        "GatewayApp configured: gateway_id=" + impl_->gateway_id_
        + " listen=" + impl_->listen_host_ + ":" + std::to_string(impl_->listen_port_)
        + " udp_listen="
        + (impl_->udp_listen_port_ == 0 ? std::string("disabled")
                                        : (impl_->udp_listen_host_ + ":" + std::to_string(impl_->udp_listen_port_)))
        + " udp_bind_ttl_ms=" + std::to_string(impl_->udp_bind_ttl_ms_)
        + " udp_bind_fail_window_ms=" + std::to_string(impl_->udp_bind_fail_window_ms_)
        + " udp_bind_fail_limit=" + std::to_string(impl_->udp_bind_fail_limit_)
        + " udp_bind_block_ms=" + std::to_string(impl_->udp_bind_block_ms_)
        + " udp_bind_retry_backoff_ms=" + std::to_string(impl_->udp_bind_retry_backoff_ms_)
        + " udp_bind_retry_backoff_max_ms=" + std::to_string(impl_->udp_bind_retry_backoff_max_ms_)
        + " udp_bind_retry_max_attempts=" + std::to_string(impl_->udp_bind_retry_max_attempts_)
        + " udp_opcode_allowlist_size=" + std::to_string(impl_->udp_opcode_allowlist_.size())
        + " udp_ingress_feature=" + std::string(kGatewayUdpIngressBuildEnabled ? "on" : "off")
        + " backend_connect_timeout_ms=" + std::to_string(impl_->backend_connect_timeout_ms_)
        + " backend_send_queue_max_bytes=" + std::to_string(impl_->backend_send_queue_max_bytes_)
        + " backend_circuit_enabled=" + std::string(impl_->backend_circuit_breaker_enabled_ ? "1" : "0")
        + " backend_circuit_fail_threshold=" + std::to_string(impl_->backend_circuit_fail_threshold_)
        + " backend_circuit_open_ms=" + std::to_string(impl_->backend_circuit_open_ms_)
        + " backend_connect_retry_budget_per_min=" + std::to_string(impl_->backend_connect_retry_budget_per_min_)
        + " backend_connect_retry_backoff_ms=" + std::to_string(impl_->backend_connect_retry_backoff_ms_)
        + " backend_connect_retry_backoff_max_ms=" + std::to_string(impl_->backend_connect_retry_backoff_max_ms_)
        + " ingress_tokens_per_sec=" + std::to_string(impl_->ingress_tokens_per_sec_)
        + " ingress_burst_tokens=" + std::to_string(impl_->ingress_burst_tokens_)
        + " ingress_max_active_sessions=" + std::to_string(impl_->ingress_max_active_sessions_)
        + " allow_anonymous=" + std::string(impl_->allow_anonymous_ ? "1" : "0")
        + " rudp_core_build=" + std::string(kCoreRudpBuildEnabled ? "1" : "0")
        + " rudp_enable=" + std::string(impl_->rudp_rollout_policy_.enabled ? "1" : "0")
        + " rudp_canary_percent=" + std::to_string(impl_->rudp_rollout_policy_.canary_percent)
        + " rudp_opcode_allowlist_size=" + std::to_string(impl_->rudp_rollout_policy_.opcode_allowlist.size())
        + " rudp_handshake_timeout_ms=" + std::to_string(impl_->rudp_config_->handshake_timeout_ms)
        + " rudp_idle_timeout_ms=" + std::to_string(impl_->rudp_config_->idle_timeout_ms)
        + " rudp_ack_delay_ms=" + std::to_string(impl_->rudp_config_->ack_delay_ms)
        + " rudp_rto_min_ms=" + std::to_string(impl_->rudp_config_->rto_min_ms)
        + " rudp_rto_max_ms=" + std::to_string(impl_->rudp_config_->rto_max_ms)
        + " rudp_max_inflight_packets=" + std::to_string(impl_->rudp_config_->max_inflight_packets)
        + " rudp_max_inflight_bytes=" + std::to_string(impl_->rudp_config_->max_inflight_bytes)
        + " rudp_mtu_payload_bytes=" + std::to_string(impl_->rudp_config_->mtu_payload_bytes));
}

void GatewayAppAccess::configure_infrastructure(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    const char* redis_env = std::getenv(kEnvRedisUri);
    impl_->redis_uri_ = redis_env ? redis_env : kDefaultRedisUri;
    if (const char* prefix = std::getenv(kEnvSessionContinuityRedisPrefix); prefix && *prefix) {
        impl_->continuity_prefix_ = prefix;
    } else if (const char* prefix = std::getenv(kEnvRedisChannelPrefix); prefix && *prefix) {
        impl_->continuity_prefix_ = prefix;
    } else {
        impl_->continuity_prefix_.clear();
    }
    if (!impl_->continuity_prefix_.empty() && impl_->continuity_prefix_.back() != ':') {
        impl_->continuity_prefix_.push_back(':');
    }
    impl_->continuity_prefix_ += "continuity:";
    impl_->session_directory_prefix_ = kDefaultSessionDirectoryPrefix;
    if (!impl_->session_directory_prefix_.empty() && impl_->session_directory_prefix_.back() != '/') {
        impl_->session_directory_prefix_.push_back('/');
    }
    impl_->resume_locator_prefix_ = impl_->session_directory_prefix_ + "locator/";

    if (const char* ttl_env = std::getenv(kEnvSessionContinuityLeaseTtlSec); ttl_env && *ttl_env) {
        try {
            const auto parsed = std::stoul(ttl_env);
            if (parsed > 0) {
                impl_->resume_locator_ttl_sec_ = static_cast<std::uint32_t>(parsed);
            }
        } catch (...) {
            server::core::log::warn("GatewayApp invalid SESSION_CONTINUITY_LEASE_TTL_SEC; using default");
            impl_->resume_locator_ttl_sec_ = kDefaultResumeLocatorTtlSec;
        }
    } else {
        impl_->resume_locator_ttl_sec_ = kDefaultResumeLocatorTtlSec;
    }

    try {
        server::core::storage::redis::Options opts;
        impl_->redis_client_ = gateway::make_redis_client(impl_->redis_uri_, opts);

        if (impl_->redis_client_) {
            std::string registry_prefix = kDefaultServerRegistryPrefix;
            if (const char* v = std::getenv(kEnvServerRegistryPrefix); v && *v) {
                registry_prefix = v;
            }

            std::chrono::seconds registry_ttl{30};
            if (const char* v = std::getenv(kEnvServerRegistryTtl); v && *v) {
                try {
                    const auto parsed = std::stoul(v);
                    if (parsed > 0) {
                        registry_ttl = std::chrono::seconds{static_cast<long long>(parsed)};
                    }
                } catch (...) {
                    server::core::log::warn("GatewayApp invalid SERVER_REGISTRY_TTL; using default");
                }
            }

            impl_->backend_registry_ =
                make_registry_backend(impl_->redis_client_, std::move(registry_prefix), registry_ttl);

            impl_->session_directory_ = std::make_unique<SessionDirectory>(
                impl_->redis_client_,
                impl_->session_directory_prefix_,
                std::chrono::seconds(600));

            server::core::log::info("GatewayApp Redis client initialised");
        } else {
            server::core::log::error("GatewayApp failed to create Redis client (REDIS_URI redacted)");
        }
    } catch (const std::exception& e) {
        server::core::log::error(std::string("GatewayApp infrastructure init failed: ") + e.what());
    }
}

} // namespace gateway
