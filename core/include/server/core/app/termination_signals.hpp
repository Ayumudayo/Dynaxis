#pragma once

#include <csignal>

namespace server::core::app {

namespace detail {

inline volatile std::sig_atomic_t g_termination_signal_received = 0;

inline void termination_signal_handler(int) noexcept {
    g_termination_signal_received = 1;
}

} // namespace detail

/**
 * @brief 프로세스 전역 종료 시그널 핸들러를 설치합니다(best-effort).
 *
 * 설계 이유:
 * - 시그널 핸들러 내부에서는 lock/할당/로그처럼 async-signal-safe 하지 않은 작업을 피해야 합니다.
 * - 따라서 핸들러는 `sig_atomic_t` 플래그만 세우고,
 *   실제 정리(shutdown step 실행)는 메인 루프/이벤트 루프에서 수행합니다.
 */
inline void install_termination_signal_handlers() noexcept {
#if defined(SIGINT)
    std::signal(SIGINT, detail::termination_signal_handler);
#endif
#if defined(SIGTERM)
    std::signal(SIGTERM, detail::termination_signal_handler);
#endif
}

/**
 * @brief 종료 시그널 수신 여부를 조회합니다.
 * @return 종료 시그널이 수신되었으면 true
 */
inline bool termination_signal_received() noexcept {
    return detail::g_termination_signal_received != 0;
}

} // namespace server::core::app
