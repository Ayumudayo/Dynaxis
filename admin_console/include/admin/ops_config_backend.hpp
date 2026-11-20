#pragma once

#include "admin/admin_types.hpp"
#include "admin/chat_ops_backend.hpp"
#include "admin/inventory_poller.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dynaxis::admin {

/**
 * @brief 서비스 재시작 허용 목록·확인 문자열·동료 인스턴스 가용성 평가 결과입니다.
 */
struct RestartValidationResult {
    bool ok{false};
    int http_status{400};
    std::string error_code;
    std::string message;
    std::vector<std::string> affected_services;
    std::string blast_radius_json{"{}"};
};

/**
 * @brief 스키마 기반 설정 패치 검증 및 Before/After diff 계산 결과입니다.
 */
struct ConfigValidationResult {
    bool ok{false};
    int http_status{400};
    std::string error_code;
    std::string message;
    std::string apply_mode{"runtime"};
    bool touches_runtime{false};
    bool touches_restart_required{false};
    std::string next_values_json{"{}"};
    std::string diff_summary_json{"[]"};
    std::string managed_env_json{"{}"};
    std::vector<std::string> affected_services;
};

/**
 * @brief 비동기 작업의 사후 `/readyz` 검증(`verifying`) 판정 결과입니다.
 */
enum class VerifyProbeState {
    PENDING,
    SUCCEEDED,
    TIMED_OUT
};

/**
 * @brief `service_restart` 대상이 카탈로그 허용 목록, 대상 확인 문자열, 동료 준비 조건을 만족하는지 검증합니다.
 * @param poller 현재 서비스 카탈로그와 최신 스냅샷을 보유한 `InventoryPoller`
 * @param target_service 재시작을 요청한 서비스 식별자 (예: `server-1`, `gateway-1`, `wb_worker`)
 * @param confirmation_target 2단계 확인으로 사용자가 재입력한 대상 서비스 식별자
 * @param now_unix_ms 현재 UNIX 밀리초 타임스탬프(`0`이면 현재 시스템 시각 사용)
 * @return 검증 통과 여부, 영향 서비스 목록, Blast Radius 요약 또는 거부 HTTP 상태
 */
RestartValidationResult evaluate_restart_request(const InventoryPoller& poller,
                                                 std::string_view target_service,
                                                 std::string_view confirmation_target,
                                                 std::uint64_t now_unix_ms = 0);

/**
 * @brief 재시작 허용 서비스 목록(`gateway-*`, `server-*`, `wb_worker`)과 동료 준비 상태 기반 실행 가능 여부를 직렬화합니다.
 * @param poller 현재 서비스 카탈로그와 최신 스냅샷을 보유한 `InventoryPoller`
 * @param has_active_operation 현재 `queued`/`running`/`verifying` 중인 선행 작업이 존재하는지 여부
 * @return 서비스별 상태, 예상 세션 수, 동료 가용성, 차단 사유를 담은 JSON 배열 문자열
 */
std::string build_allowlisted_targets_json(const InventoryPoller& poller, bool has_active_operation);

/**
 * @brief 기본 비비밀 설정 7종(`admin.*`, 세션 연속성, 드레인, 런타임 트레이싱)의 JSON 직렬화 값을 반환합니다.
 * @param config 기동 시 설정이 반영된 `AdminConfig`
 * @return 기본 비비밀 설정 키/값 JSON 객체 문자열
 */
std::string build_default_config_values_json(const AdminConfig& config);

/**
 * @brief v3 스키마 메타데이터, 비노출 `secret_refs` 마스킹 상태, 현재 설정값을 직렬화합니다.
 * @param config `secret_ref` 구성 여부 판단에 사용하는 `AdminConfig`
 * @param current_revision 현재 활성 설정 리비전 번호
 * @param current_values_json 현재 비비밀 설정 JSON 문자열
 * @param revisions_json 최근 설정 리비전 이력 JSON 배열 문자열
 * @return `GET /api/v1/config` 응답 본문에 사용할 JSON 문자열
 */
std::string build_config_overview_json(const AdminConfig& config,
                                       std::int64_t current_revision,
                                       const std::string& current_values_json,
                                       const std::string& revisions_json = "[]");

/**
 * @brief 설정 패치 본문(`values`)과 `confirmation_target`을 스키마·범위·`secret_ref` 차단 기준으로 검증합니다.
 * @param poller 영향 서비스 목록 계산에 사용하는 `InventoryPoller`
 * @param current_values_json 현재 리비전의 비비밀 설정 JSON 객체 문자열
 * @param patch_values_json 적용할 패치 키/값 JSON 객체 문자열
 * @param confirmation_target 2단계 확인용 문자열 (`"stack-config"` 필수)
 * @return 검증 결과, 정규화된 다음 설정 JSON, Before/After diff 및 `managed_env`
 */
ConfigValidationResult evaluate_config_patch(const InventoryPoller& poller,
                                             const std::string& current_values_json,
                                             const std::string& patch_values_json,
                                             std::string_view confirmation_target);

/**
 * @brief `verifying` 상태 작업의 영향 서비스가 `/readyz`를 회복했는지 또는 검증 기한을 초과했는지 판정합니다.
 * @param poller 최신 관측 스냅샷을 제공하는 `InventoryPoller`
 * @param affected_services 검증 대상 서비스 식별자 목록
 * @param verifying_since_unix_ms 호스트 어댑터 명령 완료 후 `verifying` 단계에 진입한 UNIX ms 시각
 * @param verify_deadline_unix_ms `/readyz` 회복 검증 만료 UNIX ms 시각
 * @param now_unix_ms 현재 UNIX ms 타임스탬프(`0`이면 현재 시스템 시각 사용)
 * @return `SUCCEEDED`, `TIMED_OUT`, 또는 아직 회복 대기 중이면 `PENDING`
 */
VerifyProbeState evaluate_verifying_services(const InventoryPoller& poller,
                                             const std::vector<std::string>& affected_services,
                                             std::uint64_t verifying_since_unix_ms,
                                             std::uint64_t verify_deadline_unix_ms,
                                             std::uint64_t now_unix_ms = 0);

/**
 * @brief 웹 관리 콘솔 v3의 비동기 작업(서비스 재시작) 및 스키마 기반 설정/롤백 저장소 인터페이스입니다.
 *
 * `admin_app` HTTP 핸들러는 호스트 Docker 소켓에 직접 접근하지 않고 본 인터페이스를 통해
 * PostgreSQL 원장에 작업 의도를 기록(`202 Accepted`)하며, 호스트 실행 어댑터가 작업을 claim해 수행합니다.
 */
class IOpsConfigBackend {
public:
    /** @brief 다형성 소멸을 보장하는 기본 가상 소멸자입니다. */
    virtual ~IOpsConfigBackend() = default;

    /**
     * @brief 최근 비동기 운영 작업(`admin_operations`) 목록을 최신 역순으로 조회합니다.
     * @param status_filter 상태 필터 (`queued`/`running`/`verifying`/`succeeded`/`failed`/`timed_out`, 비면 전체)
     * @param limit 최대 반환 개수
     * @return HTTP 상태 코드 및 작업 목록 JSON
     */
    virtual ChatOpHttpResult list_operations(std::string_view status_filter, std::size_t limit) = 0;

    /**
     * @brief 단일 비동기 작업 상세와 4단계 진행 스테퍼 상태를 조회합니다.
     * @param operation_id 조회할 작업 UUID
     * @param refresh_probe `verifying` 상태일 때 즉시 1회 `/readyz` 재관측을 수행할지 여부
     * @return HTTP 상태 코드 및 단일 작업 상세 JSON (없는 ID면 404)
     */
    virtual ChatOpHttpResult get_operation(std::string_view operation_id, bool refresh_probe) = 0;

    /**
     * @brief 허용 목록에 포함된 서비스의 비동기 재시작 작업을 큐에 접수(`202 Accepted`)합니다.
     * @param target_service 재시작 대상 서비스 식별자
     * @param confirmation_target 2단계 확인 문자열 (`target_service`와 일치해야 함)
     * @param reason 감사 로그 사유
     * @param idempotency_key 멱등성 보장 키
     * @param actor_role 요청자 역할 (`AdminRole::ADMIN` 전용)
     * @return HTTP `202 Accepted` 또는 검증/충돌(`400`/`409`) JSON
     */
    virtual ChatOpHttpResult create_restart_operation(std::string_view target_service,
                                                      std::string_view confirmation_target,
                                                      std::string_view reason,
                                                      std::string_view idempotency_key,
                                                      AdminRole actor_role) = 0;

    /**
     * @brief 호스트 실행 어댑터가 가장 오래된 `queued` 작업을 원자적으로 `running`으로 claim합니다.
     * @param actor_role 호출자 역할 (`AdminRole::ADMIN` 전용)
     * @return `claimed` 여부와 작업·실행 계획 JSON
     */
    virtual ChatOpHttpResult claim_next_queued_operation(AdminRole actor_role) = 0;

    /**
     * @brief 호스트 실행 어댑터가 `running` 작업의 명령 실행 결과를 보고해 `verifying` 또는 `failed`로 전이시킵니다.
     * @param operation_id 대상 작업 UUID
     * @param outcome `"command_succeeded"` 또는 `"command_failed"`
     * @param summary 비밀이 제거된 명령 실행 결과 요약 문자열
     * @param actor_role 호출자 역할 (`AdminRole::ADMIN` 전용)
     * @return 전이된 작업 상세 JSON
     */
    virtual ChatOpHttpResult report_worker_result(std::string_view operation_id,
                                                  std::string_view outcome,
                                                  std::string_view summary,
                                                  AdminRole actor_role) = 0;

    /**
     * @brief 현재 설정 스키마, 활성 리비전, `secret_refs` 마스킹 상태, 최근 리비전 이력을 조회합니다.
     * @return HTTP `200 OK` 및 설정 스키마/리비전 JSON
     */
    virtual ChatOpHttpResult get_config() = 0;

    /**
     * @brief 스키마로 검증된 비비밀 설정 패치를 적용해 리비전을 생성하고 필요한 경우 재시작 작업을 큐에 접수합니다.
     * @param raw_body `expected_revision`, `values`, `confirmation_target`, `reason`, `idempotency_key`를 담은 JSON 본문
     * @param reason 라우터가 파싱한 감사 사유 (본문과 동일)
     * @param idempotency_key 라우터가 파싱한 멱등 키
     * @param actor_role 요청자 역할 (`AdminRole::ADMIN` 전용)
     * @return HTTP `202 Accepted` 또는 검증/충돌(`400`/`409`) JSON
     */
    virtual ChatOpHttpResult apply_config(const std::string& raw_body,
                                          std::string_view reason,
                                          std::string_view idempotency_key,
                                          AdminRole actor_role) = 0;

    /**
     * @brief 과거 설정 리비전으로 되돌리는 새 리비전과 필요한 경우 재시작 작업을 큐에 접수합니다.
     * @param target_revision 복구 기준 리비전 번호
     * @param confirmation_target 2단계 확인 문자열 (`"stack-config"` 필수)
     * @param reason 감사 사유
     * @param idempotency_key 멱등 키
     * @param actor_role 요청자 역할 (`AdminRole::ADMIN` 전용)
     * @return HTTP `202 Accepted` 또는 검증/충돌(`400`/`404`/`409`) JSON
     */
    virtual ChatOpHttpResult rollback_config(std::int64_t target_revision,
                                             std::string_view confirmation_target,
                                             std::string_view reason,
                                             std::string_view idempotency_key,
                                             AdminRole actor_role) = 0;

    /**
     * @brief 권한 부족이나 읽기 전용 모드로 차단된 v3 변경 시도를 감사 이력에 남깁니다.
     * @param actor_role 요청자 역할
     * @param capability 요구된 기능 영역
     * @param action 시도된 작업명
     * @param target 대상 식별자
     * @param reason 전달된 사유
     * @param idempotency_key 전달된 멱등 키
     * @param deny_code 거절 사유 코드
     */
    virtual void record_denied_audit(AdminRole actor_role,
                                     std::string_view capability,
                                     std::string_view action,
                                     std::string_view target,
                                     std::string_view reason,
                                     std::string_view idempotency_key,
                                     std::string_view deny_code) noexcept = 0;
};

/**
 * @brief PostgreSQL 원장(`admin_operations`, `admin_config_revisions`)과 `InventoryPoller`를 연동하는 v3 운영 백엔드입니다.
 */
class PostgresOpsConfigBackend final : public IOpsConfigBackend {
public:
    /**
     * @brief 설정 및 서비스 카탈로그/런타임 튜닝 핸들(`InventoryPoller`)로 v3 백엔드를 초기화합니다.
     * @param config DB URI 및 검증 timeout 설정을 담은 `AdminConfig`
     * @param poller 서비스 카탈로그 조회, `/readyz` 사후 검증, 런타임 설정 즉시 반영을 담당하는 `InventoryPoller` 참조
     */
    PostgresOpsConfigBackend(AdminConfig config, InventoryPoller& poller);

    ChatOpHttpResult list_operations(std::string_view status_filter, std::size_t limit) override;
    ChatOpHttpResult get_operation(std::string_view operation_id, bool refresh_probe) override;
    ChatOpHttpResult create_restart_operation(std::string_view target_service,
                                              std::string_view confirmation_target,
                                              std::string_view reason,
                                              std::string_view idempotency_key,
                                              AdminRole actor_role) override;
    ChatOpHttpResult claim_next_queued_operation(AdminRole actor_role) override;
    ChatOpHttpResult report_worker_result(std::string_view operation_id,
                                          std::string_view outcome,
                                          std::string_view summary,
                                          AdminRole actor_role) override;
    ChatOpHttpResult get_config() override;
    ChatOpHttpResult apply_config(const std::string& raw_body,
                                  std::string_view reason,
                                  std::string_view idempotency_key,
                                  AdminRole actor_role) override;
    ChatOpHttpResult rollback_config(std::int64_t target_revision,
                                     std::string_view confirmation_target,
                                     std::string_view reason,
                                     std::string_view idempotency_key,
                                     AdminRole actor_role) override;
    void record_denied_audit(AdminRole actor_role,
                             std::string_view capability,
                             std::string_view action,
                             std::string_view target,
                             std::string_view reason,
                             std::string_view idempotency_key,
                             std::string_view deny_code) noexcept override;

private:
    AdminConfig config_;
    InventoryPoller& poller_;
};

} // namespace dynaxis::admin
