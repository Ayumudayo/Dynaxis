#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dynaxis::admin {

/**
 * @brief 서비스 카탈로그에서 다루는 워크로드 성격 구분입니다.
 */
enum class ServiceKind {
    LONG_RUNNING,
    INFRASTRUCTURE,
    ONE_SHOT
};

/**
 * @brief 토폴로지 스트립(`Overview`)에서 서비스를 배치할 계층 구분입니다.
 */
enum class TopologyLayer {
    EDGE,
    GATEWAY,
    SERVER,
    WORKER,
    DATA,
    OBSERVABILITY
};

/**
 * @brief 5종 운영 상태 시맨틱 코드입니다.
 *
 * `migrator` 같은 일회성 작업의 정상 종료(`COMPLETED`) 및 관측 TTL 만료/타임아웃(`UNKNOWN`)을
 * 상시 장애(`NOT_READY`)와 엄격히 구분합니다.
 */
enum class ServiceStatus {
    READY,
    DEGRADED,
    NOT_READY,
    COMPLETED,
    UNKNOWN
};

/**
 * @brief 웹 관리 콘솔 접근 권한 역할입니다.
 */
enum class AdminRole {
    VIEWER,
    OPERATOR,
    ADMIN
};

/**
 * @brief 제어면 HTTP API의 인증 모드입니다.
 */
enum class AdminAuthMode {
    TOKEN,
    DEVELOPMENT
};

/**
 * @brief 대상 서비스에 적용하는 bounded probe 유형입니다.
 */
enum class ProbeType {
    HTTP_HEALTH_READY,
    HTTP_OK,
    REDIS_RESP,
    POSTGRES_SQL,
    POSTGRES_MIGRATOR
};

/**
 * @brief 신뢰할 수 있는 사전 등록 서비스 카탈로그의 개별 대상 정의입니다.
 *
 * 브라우저 입력 URL이 아닌 서버 기동 카탈로그/토폴로지에서만 결정되어 SSRF를 차단합니다.
 */
struct ServiceTargetConfig {
    std::string service_id;
    std::string display_name;
    ServiceKind kind{ServiceKind::LONG_RUNNING};
    TopologyLayer layer{TopologyLayer::SERVER};
    ProbeType probe_type{ProbeType::HTTP_HEALTH_READY};
    std::string host;
    unsigned short port{0};
    std::string health_path{"/healthz"};
    std::string ready_path{"/readyz"};
    std::string metrics_path{"/metrics"};
    int desired_count{1};
    bool optional_profile{false};
    std::string grafana_url;
    std::string external_url;
};

/**
 * @brief 단일 서비스 대상에 대한 최신 관측 스냅샷입니다.
 */
struct ServiceObservation {
    std::string service_id;
    std::string display_name;
    ServiceKind kind{ServiceKind::LONG_RUNNING};
    TopologyLayer layer{TopologyLayer::SERVER};
    int desired_count{1};
    int ready_count{0};
    ServiceStatus status{ServiceStatus::UNKNOWN};
    bool health_ok{false};
    bool ready_ok{false};
    std::uint32_t probe_latency_ms{0};
    std::uint64_t observed_at_unix_ms{0};
    std::string detail;
    std::int64_t active_sessions{-1};
    std::int64_t wb_pending{-1};
    std::int64_t wb_dlq_count{-1};
    std::uint64_t wb_dlq_total{0};
    std::int64_t active_rooms{-1};
    std::int64_t applied_migrations{-1};
    std::int64_t expected_migrations{-1};
    std::string grafana_url;
    std::string external_url;
};

/**
 * @brief 전체 인벤토리 폴러가 원자적으로 발행하는 v1 상태 스냅샷입니다.
 */
struct InventorySnapshot {
    std::uint64_t captured_at_unix_ms{0};
    std::vector<ServiceObservation> services;
    int ready_long_running{0};
    int desired_long_running{0};
    std::uint64_t gateway_sessions{0};
    std::uint64_t server_sessions{0};
    std::int64_t active_rooms{-1};
    std::int64_t wb_pending{-1};
    std::int64_t wb_dlq_count{-1};
    std::uint64_t wb_dlq_total{0};
};

/**
 * @brief `admin_app` 기동 및 폴링·보안 정책을 담는 설정 구조체입니다.
 */
struct AdminConfig {
    unsigned short listen_port{9200};
    std::string env_label{"LOCAL"};
    bool read_only{true};
    AdminAuthMode auth_mode{AdminAuthMode::TOKEN};
    AdminRole dev_default_role{AdminRole::VIEWER};
    std::string viewer_token;
    std::string operator_token;
    std::string admin_token;
    std::filesystem::path web_root{"admin_console/web"};
    std::filesystem::path topology_path;
    std::filesystem::path migrations_dir{"tools/migrations"};
    std::string target_mode{"host"};
    std::uint32_t poll_interval_ms{5000};
    std::uint32_t probe_timeout_ms{1500};
    std::uint64_t stale_ttl_ms{15000};
    std::int64_t wb_pending_warn{500};
    bool observability_enabled{false};
    std::string grafana_base_url{"http://127.0.0.1:33000"};
    std::string prometheus_base_url{"http://127.0.0.1:39090"};
    std::string haproxy_stats_url{"http://127.0.0.1:8404/"};
    std::string db_uri;
    std::string redis_uri;
    std::string redis_channel_prefix{"dynaxis:"};
    std::string wb_stream_key{"session_events"};
    std::string dlq_stream_key{"session_events_dlq"};
    std::string dead_stream_key{"session_events_dead"};
    std::string dlq_group{"wb_dlq_group"};
    std::uint32_t dlq_retry_max{5};
    unsigned short redis_host_port{36379};
    unsigned short postgres_host_port{35432};
    unsigned short haproxy_stats_host_port{8404};
    unsigned short gateway1_metrics_host_port{36001};
    unsigned short gateway2_metrics_host_port{36002};
    unsigned short wb_worker_metrics_host_port{39093};
    unsigned short prometheus_host_port{39090};
    unsigned short grafana_host_port{33000};
    unsigned short redis_exporter_host_port{39121};
    unsigned short postgres_exporter_host_port{39187};
};

/**
 * @brief `ServiceKind` 열거형을 API 직렬화용 문자열로 변환합니다.
 * @param kind 변환할 `ServiceKind`
 * @return `"long_running"` / `"infrastructure"` / `"one_shot"` 문자열
 */
inline constexpr std::string_view service_kind_to_string(ServiceKind kind) noexcept {
    switch (kind) {
        case ServiceKind::LONG_RUNNING: return "long_running";
        case ServiceKind::INFRASTRUCTURE: return "infrastructure";
        case ServiceKind::ONE_SHOT: return "one_shot";
    }
    return "long_running";
}

/**
 * @brief `TopologyLayer` 열거형을 API 직렬화용 문자열로 변환합니다.
 * @param layer 변환할 `TopologyLayer`
 * @return 소문자 계층 문자열
 */
inline constexpr std::string_view topology_layer_to_string(TopologyLayer layer) noexcept {
    switch (layer) {
        case TopologyLayer::EDGE: return "edge";
        case TopologyLayer::GATEWAY: return "gateway";
        case TopologyLayer::SERVER: return "server";
        case TopologyLayer::WORKER: return "worker";
        case TopologyLayer::DATA: return "data";
        case TopologyLayer::OBSERVABILITY: return "observability";
    }
    return "server";
}

/**
 * @brief `ServiceStatus` 열거형을 API 직렬화용 문자열로 변환합니다.
 * @param status 변환할 `ServiceStatus`
 * @return `"ready"` / `"degraded"` / `"not_ready"` / `"completed"` / `"unknown"` 문자열
 */
inline constexpr std::string_view service_status_to_string(ServiceStatus status) noexcept {
    switch (status) {
        case ServiceStatus::READY: return "ready";
        case ServiceStatus::DEGRADED: return "degraded";
        case ServiceStatus::NOT_READY: return "not_ready";
        case ServiceStatus::COMPLETED: return "completed";
        case ServiceStatus::UNKNOWN: return "unknown";
    }
    return "unknown";
}

/**
 * @brief `AdminRole` 열거형을 소문자 문자열로 변환합니다.
 * @param role 변환할 `AdminRole`
 * @return `"viewer"` / `"operator"` / `"admin"` 문자열
 */
inline constexpr std::string_view admin_role_to_string(AdminRole role) noexcept {
    switch (role) {
        case AdminRole::VIEWER: return "viewer";
        case AdminRole::OPERATOR: return "operator";
        case AdminRole::ADMIN: return "admin";
    }
    return "viewer";
}

/**
 * @brief 관측 시각과 TTL을 검사해 stale 상태를 `UNKNOWN`으로 강등합니다.
 * @param obs 판정할 서비스 관측 결과
 * @param now_unix_ms 현재 UNIX 밀리초 타임스탬프
 * @param stale_ttl_ms 허용할 최대 관측 경과 시간(ms)
 * @return TTL 내 유효 샘플이면 `obs.status`, 만료 또는 미관측이면 `ServiceStatus::UNKNOWN`
 */
inline ServiceStatus evaluate_effective_status(const ServiceObservation& obs,
                                               std::uint64_t now_unix_ms,
                                               std::uint64_t stale_ttl_ms) noexcept {
    if (obs.observed_at_unix_ms == 0) {
        return ServiceStatus::UNKNOWN;
    }
    if (stale_ttl_ms > 0 &&
        now_unix_ms > obs.observed_at_unix_ms &&
        (now_unix_ms - obs.observed_at_unix_ms) > stale_ttl_ms) {
        return ServiceStatus::UNKNOWN;
    }
    return obs.status;
}

} // namespace dynaxis::admin
