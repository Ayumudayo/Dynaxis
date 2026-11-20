#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace server::app::chat {

/** @brief 재접속 lease와 room 복원의 성공/실패 집계입니다. */
struct ContinuityMetrics {
    std::uint64_t lease_issue_total{0};
    std::uint64_t lease_issue_fail_total{0};
    std::uint64_t lease_resume_total{0};
    std::uint64_t lease_resume_fail_total{0};
    std::uint64_t state_write_total{0};
    std::uint64_t state_write_fail_total{0};
    std::uint64_t state_restore_total{0};
    std::uint64_t state_restore_fallback_total{0};
};

} // namespace server::app::chat
