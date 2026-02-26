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
 *
 * 계약:
 * - 호출은 idempotent에 가깝게 취급됩니다(여러 번 호출해도 동일 핸들러 재등록).
 * - 핸들러는 프로세스 전역 상태를 사용하므로, 라이브러리 소비자는
 *   `termination_signal_received()` 폴링 또는 `AppHost` 경유 종료 경로 중
 *   하나를 일관되게 선택해야 합니다.
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
 *
 * 계약:
 * - 프로세스 시작 이후 한 번이라도 종료 시그널이 수신되면 true를 유지합니다.
 * - 반환값은 lock-free polling 용도이며, 상세 종료 절차는 호출자가 수행합니다.
 */
inline bool termination_signal_received() noexcept {
    return detail::g_termination_signal_received != 0;
}

} // namespace server::core::app
