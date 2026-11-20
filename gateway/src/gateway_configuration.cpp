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
#include "gateway/session_directory.hpp"
#include "signed_authenticator.hpp"
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
constexpr std::uint32_t kDefaultResumeLocatorTtlSec = 900;

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

    impl_->allow_anonymous_ = true;
    const auto auth_config = server::auth::load_config_from_env();
    impl_->authenticator_ = auth_config.required
        ? std::shared_ptr<auth::IAuthenticator>(std::make_shared<auth::SignedAuthenticator>(auth_config))
        : std::shared_ptr<auth::IAuthenticator>(std::make_shared<auth::NoopAuthenticator>());
    server::core::log::info(auth_config.required ? "Gateway signed authentication enabled" : "Gateway development authentication enabled");
    if (const char* anonymous_env = std::getenv(kEnvAllowAnonymous); anonymous_env && *anonymous_env) {
        impl_->allow_anonymous_ = (std::string_view(anonymous_env) != "0");
    }

    server::core::log::info(
        "GatewayApp configured: gateway_id=" + impl_->gateway_id_
        + " listen=" + impl_->listen_host_ + ":" + std::to_string(impl_->listen_port_)
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
        + " allow_anonymous=" + std::string(impl_->allow_anonymous_ ? "1" : "0"));
}

void GatewayAppAccess::configure_infrastructure(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    const char* redis_env = std::getenv(kEnvRedisUri);
    impl_->redis_uri_ = redis_env ? redis_env : kDefaultRedisUri;
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
