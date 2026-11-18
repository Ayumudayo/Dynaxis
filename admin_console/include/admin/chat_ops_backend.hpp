#pragma once

#include "admin/admin_types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace dynaxis::admin {

/**
 * @brief v2 채팅 운영·제재·DLQ·감사 API 핸들러가 반환하는 HTTP 상태 코드와 JSON 본문입니다.
 */
struct ChatOpHttpResult {
    int http_status{200};
    std::string json_body;
};

/**
 * @brief 웹 관리 콘솔 v2의 채팅 도메인 조회·제어·감사 저장소 인터페이스입니다.
 *
 * 실제 운영에서는 `PostgresRedisChatOpsBackend`가 PostgreSQL(`chat_shared_state`,
 * `rooms`, `users`, `sessions`, `admin_sanctions`, `admin_audit_logs`, `admin_idempotency_keys`)과
 * Redis(`presence`, `continuity`, `rooms:active`, `fanout:control`, DLQ 스트림)를 연동하고,
 * 단위 테스트에서는 가짜(Fake) 구현을 주입해 RBAC·멱등성·프라이버시 경계를 검증합니다.
 */
class IChatOpsBackend {
public:
    /** @brief 다형성 소멸을 보장하는 기본 가상 소멸자입니다. */
    virtual ~IChatOpsBackend() = default;

    /**
     * @brief 활성/종료 채팅방 목록과 참여자 수, `watermark` 요약을 조회합니다.
     * @param query 방 이름 부분 검색어
     * @param include_closed 종료된 방 포함 여부
     * @param limit 최대 반환 개수
     * @return HTTP 상태 코드 및 JSON 응답 문자열 (채팅 본문 평문 미포함)
     */
    virtual ChatOpHttpResult list_rooms(std::string_view query, bool include_closed, std::size_t limit) = 0;

    /**
     * @brief 단일 방의 메타데이터, 참여자, `watermark` 상세를 조회합니다.
     * @param room_name 조회할 방 이름
     * @return HTTP 상태 코드 및 방 상세 JSON (없는 방이면 404)
     */
    virtual ChatOpHttpResult get_room_detail(std::string_view room_name) = 0;

    /**
     * @brief 관리자가 채팅방을 강제 종료(`active=false`)하고 제어 변경 알림을 전파합니다.
     * @param room_name 종료할 방 이름 (`lobby` 금지)
     * @param reason 감사 사유
     * @param idempotency_key 중복 실행 방지 멱등 키
     * @param actor_role 요청을 수행한 운영자 역할
     * @return HTTP 상태 코드 및 결과 JSON (`409` 멱등 충돌 포함)
     */
    virtual ChatOpHttpResult close_room(std::string_view room_name,
                                        std::string_view reason,
                                        std::string_view idempotency_key,
                                        AdminRole actor_role) = 0;

    /**
     * @brief 사용자 검색어 또는 최근 활성 로그인 기준으로 사용자 목록을 조회합니다.
     * @param query 소문자 ASCII `subject` 검색어
     * @param limit 최대 반환 개수
     * @return HTTP 상태 코드 및 사용자 목록 JSON (토큰 원문 미포함)
     */
    virtual ChatOpHttpResult list_users(std::string_view query, std::size_t limit) = 0;

    /**
     * @brief 단일 사용자의 Postgres Login Lease와 Redis Presence TTL 진실 소스를 나란히 대조합니다.
     * @param subject 조회할 사용자 `subject` (`principal_id`)
     * @return HTTP 상태 코드 및 양측 진실 소스 대조 JSON (없는 대상이면 404)
     */
    virtual ChatOpHttpResult get_user_detail(std::string_view subject) = 0;

    /**
     * @brief 사용자의 로그인 예약(`login:<subject>`)과 세션을 즉시 회수해 전역 강제 종료를 수행합니다.
     * @param subject 강제 종료할 사용자 `subject`
     * @param reason 감사 사유
     * @param idempotency_key 멱등 키
     * @param actor_role 요청자 역할
     * @return HTTP 상태 코드 및 실행 요약 JSON
     */
    virtual ChatOpHttpResult kick_user(std::string_view subject,
                                       std::string_view reason,
                                       std::string_view idempotency_key,
                                       AdminRole actor_role) = 0;

    /**
     * @brief 제재(`mute`/`ban`) 이력 및 활성 제재 목록을 조회합니다.
     * @param active_only 만료 전·미해제 제재만 조회할지 여부
     * @param subject 특정 `subject` 필터 (비어 있으면 전체)
     * @param limit 최대 반환 개수
     * @return HTTP 상태 코드 및 제재 목록 JSON
     */
    virtual ChatOpHttpResult list_sanctions(bool active_only,
                                            std::string_view subject,
                                            std::size_t limit) = 0;

    /**
     * @brief `mute` 또는 `ban` 제재를 부여하고 `chat_shared_state`에 트랜잭션으로 반영합니다.
     * @param subject 제재 대상 사용자 `subject`
     * @param kind `"mute"` 또는 `"ban"`
     * @param duration_sec 제재 지속 시간(초)
     * @param confirm_permanent 24시간 초과 장기 제재 확인 여부
     * @param reason 감사 사유
     * @param idempotency_key 멱등 키
     * @param actor_role 요청자 역할
     * @return HTTP 상태 코드 및 생성된 제재 JSON
     */
    virtual ChatOpHttpResult create_sanction(std::string_view subject,
                                             std::string_view kind,
                                             std::uint64_t duration_sec,
                                             bool confirm_permanent,
                                             std::string_view reason,
                                             std::string_view idempotency_key,
                                             AdminRole actor_role) = 0;

    /**
     * @brief 활성 제재를 조기 해제하고 잔여 제재 만료 시각을 재계산해 전파합니다.
     * @param sanction_id 해제할 제재 UUID
     * @param reason 조기 해제 사유
     * @param idempotency_key 멱등 키
     * @param actor_role 요청자 역할
     * @return HTTP 상태 코드 및 해제 결과 JSON
     */
    virtual ChatOpHttpResult revoke_sanction(std::string_view sanction_id,
                                             std::string_view reason,
                                             std::string_view idempotency_key,
                                             AdminRole actor_role) = 0;

    /**
     * @brief Write-Behind Pending/DLQ/Dead 스트림 건수와 DLQ 항목 미리보기를 조회합니다.
     * @param limit 최대 미리보기 항목 개수
     * @return HTTP 상태 코드 및 DLQ 상태 JSON
     */
    virtual ChatOpHttpResult inspect_dlq(std::size_t limit) = 0;

    /**
     * @brief DLQ 스트림의 미처리 이벤트를 DB `session_events`로 제한 배치 재처리합니다.
     * @param max_items 한 번에 처리할 최대 항목 수
     * @param reason 재처리 감사 사유
     * @param idempotency_key 멱등 키
     * @param actor_role 요청자 역할
     * @return HTTP 상태 코드 및 처리 통계(`replayed_ok`, `retried_dlq`, `moved_dead`) JSON
     */
    virtual ChatOpHttpResult replay_dlq(std::size_t max_items,
                                        std::string_view reason,
                                        std::string_view idempotency_key,
                                        AdminRole actor_role) = 0;

    /**
     * @brief 운영 감사 로그(`admin_audit_logs`)를 최신 역순으로 조회합니다.
     * @param actor_filter 행위자 역할 필터
     * @param action_filter 액션 이름 필터
     * @param target_filter 대상 식별자 부분 필터
     * @param status_filter 상태(`succeeded`/`denied`/`conflict`/`failed`) 필터
     * @param limit 최대 반환 개수
     * @return HTTP 상태 코드 및 감사 로그 목록 JSON
     */
    virtual ChatOpHttpResult list_audit_logs(std::string_view actor_filter,
                                             std::string_view action_filter,
                                             std::string_view target_filter,
                                             std::string_view status_filter,
                                             std::size_t limit) = 0;

    /**
     * @brief 권한 부족 또는 읽기 전용 모드로 차단된 변경 시도도 감사 이력에 남깁니다.
     * @param actor_role 요청자 역할
     * @param capability 요구된 기능 영역
     * @param action 시도된 작업명
     * @param target 대상 식별자
     * @param reason 전달된 사유
     * @param idempotency_key 전달된 멱등 키
     * @param deny_code 거절 사유 코드(`read_only_mode` 등)
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
 * @brief PostgreSQL과 Redis를 직접 연동하는 `IChatOpsBackend` 운영 구현체입니다.
 */
class PostgresRedisChatOpsBackend final : public IChatOpsBackend {
public:
    /**
     * @brief DB/Redis URI 및 채널 접두사, DLQ 설정을 담은 `AdminConfig`로 백엔드를 초기화합니다.
     * @param config 관리자 콘솔 설정 구조체
     */
    explicit PostgresRedisChatOpsBackend(AdminConfig config);

    ChatOpHttpResult list_rooms(std::string_view query, bool include_closed, std::size_t limit) override;
    ChatOpHttpResult get_room_detail(std::string_view room_name) override;
    ChatOpHttpResult close_room(std::string_view room_name,
                                std::string_view reason,
                                std::string_view idempotency_key,
                                AdminRole actor_role) override;
    ChatOpHttpResult list_users(std::string_view query, std::size_t limit) override;
    ChatOpHttpResult get_user_detail(std::string_view subject) override;
    ChatOpHttpResult kick_user(std::string_view subject,
                               std::string_view reason,
                               std::string_view idempotency_key,
                               AdminRole actor_role) override;
    ChatOpHttpResult list_sanctions(bool active_only,
                                    std::string_view subject,
                                    std::size_t limit) override;
    ChatOpHttpResult create_sanction(std::string_view subject,
                                     std::string_view kind,
                                     std::uint64_t duration_sec,
                                     bool confirm_permanent,
                                     std::string_view reason,
                                     std::string_view idempotency_key,
                                     AdminRole actor_role) override;
    ChatOpHttpResult revoke_sanction(std::string_view sanction_id,
                                     std::string_view reason,
                                     std::string_view idempotency_key,
                                     AdminRole actor_role) override;
    ChatOpHttpResult inspect_dlq(std::size_t limit) override;
    ChatOpHttpResult replay_dlq(std::size_t max_items,
                                std::string_view reason,
                                std::string_view idempotency_key,
                                AdminRole actor_role) override;
    ChatOpHttpResult list_audit_logs(std::string_view actor_filter,
                                     std::string_view action_filter,
                                     std::string_view target_filter,
                                     std::string_view status_filter,
                                     std::size_t limit) override;
    void record_denied_audit(AdminRole actor_role,
                             std::string_view capability,
                             std::string_view action,
                             std::string_view target,
                             std::string_view reason,
                             std::string_view idempotency_key,
                             std::string_view deny_code) noexcept override;

private:
    AdminConfig config_;
};

} // namespace dynaxis::admin
