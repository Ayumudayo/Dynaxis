#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace server::app::chat {

/** @brief 단일 채팅 훅(hook) 플러그인 한 개의 상태와 재로드 결과를 담는 운영 스냅샷입니다. */
struct ChatHookPluginMetric {
    /** @brief 훅(hook)별 호출 수, 오류 수, 지연 분포를 담는 세부 집계입니다. */
    struct HookMetric {
        std::string hook_name;
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
        std::uint64_t duration_count{0};
        std::uint64_t duration_sum_ns{0};
        std::array<std::uint64_t, 12> duration_bucket_counts{};
    };

    std::string file;                        ///< 플러그인 파일 경로
    bool loaded{false};                      ///< 현재 로드 성공 여부
    std::string name;                        ///< 플러그인 이름
    std::string version;                     ///< 플러그인 버전 문자열
    std::uint64_t reload_attempt_total{0};  ///< reload 시도 누적 횟수
    std::uint64_t reload_success_total{0};  ///< reload 성공 누적 횟수
    std::uint64_t reload_failure_total{0};  ///< reload 실패 누적 횟수
    std::vector<HookMetric> hook_metrics;    ///< hook별 호출/에러 누적 횟수
};

/** @brief 플러그인 체인 전체를 한 번에 관찰하기 위한 집계 스냅샷입니다. */
struct ChatHookPluginsMetrics {
    bool enabled{false};                          ///< 플러그인 기능 활성화 여부
    std::string mode;                             ///< 로드 모드(`none|dir|paths|single`)
    std::vector<ChatHookPluginMetric> plugins;    ///< 플러그인별 메트릭 목록
};

/** @brief Lua 훅(hook) 하나가 얼마나 자주 실패하거나 자동 비활성화됐는지 보여 주는 집계입니다. */
struct LuaHookMetric {
    std::string hook_name;
    bool disabled{false};
    std::uint64_t consecutive_failures{0};
    std::uint64_t auto_disable_total{0};
    std::uint64_t calls_total{0};
    std::uint64_t errors_total{0};
    std::uint64_t instruction_limit_hits{0};
    std::uint64_t memory_limit_hits{0};
};

/** @brief 특정 훅(hook)과 특정 스크립트 조합의 누적 호출 결과를 담는 집계입니다. */
struct LuaScriptCallMetric {
    std::string hook_name;
    std::string script_name;
    std::uint64_t calls_total{0};
    std::uint64_t errors_total{0};
};

/** @brief Lua 런타임 전체 상태를 제어면과 메트릭 경로에서 읽기 쉽게 펼친 스냅샷입니다. */
struct LuaHooksMetrics {
    bool enabled{false};
    std::uint64_t auto_disable_threshold{0};
    std::uint64_t reload_epoch{0};
    std::size_t loaded_scripts{0};
    std::size_t memory_used_bytes{0};
    std::uint64_t calls_total{0};
    std::uint64_t errors_total{0};
    std::uint64_t instruction_limit_hits{0};
    std::uint64_t memory_limit_hits{0};
    std::vector<LuaHookMetric> hooks;
    std::vector<LuaScriptCallMetric> script_calls;
};

/** @brief continuity 임대(lease), world 복구, migration handoff의 성공/실패 누계를 묶은 집계입니다. */
struct ContinuityMetrics {
    std::uint64_t lease_issue_total{0};
    std::uint64_t lease_issue_fail_total{0};
    std::uint64_t lease_resume_total{0};
    std::uint64_t lease_resume_fail_total{0};
    std::uint64_t state_write_total{0};
    std::uint64_t state_write_fail_total{0};
    std::uint64_t state_restore_total{0};
    std::uint64_t state_restore_fallback_total{0};
    std::uint64_t world_write_total{0};
    std::uint64_t world_write_fail_total{0};
    std::uint64_t world_restore_total{0};
    std::uint64_t world_restore_fallback_total{0};
    std::uint64_t world_restore_fallback_missing_world_total{0};
    std::uint64_t world_restore_fallback_missing_owner_total{0};
    std::uint64_t world_restore_fallback_owner_mismatch_total{0};
    std::uint64_t world_restore_fallback_draining_replacement_unhonored_total{0};
    std::uint64_t world_owner_write_total{0};
    std::uint64_t world_owner_write_fail_total{0};
    std::uint64_t world_owner_restore_total{0};
    std::uint64_t world_owner_restore_fallback_total{0};
    std::uint64_t world_migration_restore_total{0};
    std::uint64_t world_migration_restore_fallback_total{0};
    std::uint64_t world_migration_restore_fallback_target_world_missing_total{0};
    std::uint64_t world_migration_restore_fallback_target_owner_missing_total{0};
    std::uint64_t world_migration_restore_fallback_target_owner_not_ready_total{0};
    std::uint64_t world_migration_restore_fallback_target_owner_mismatch_total{0};
    std::uint64_t world_migration_restore_fallback_source_not_draining_total{0};
    std::uint64_t world_migration_payload_room_handoff_total{0};
    std::uint64_t world_migration_payload_room_handoff_fallback_total{0};
};

} // namespace server::app::chat
