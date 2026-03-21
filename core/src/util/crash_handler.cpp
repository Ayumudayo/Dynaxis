#include "server/core/util/crash_handler.hpp"

#include "server/core/util/log.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#  include <windows.h>
#  include <DbgHelp.h>
#else
#  include <execinfo.h>
#  include <signal.h>
#  include <unistd.h>
#endif

/**
 * @brief 프로세스 크래시 시 최소 정보를 안전하게 남기는 핸들러 구현입니다.
 *
 * signal-safe 범위에서만 출력/스택덤프를 수행해,
 * 장애 원인 추적 단서를 남기면서 2차 크래시 위험을 줄입니다.
 */
namespace server::core::util::crash {

namespace {
std::atomic<bool> g_installed{false};

void safe_write_literal(const char* text) noexcept {
    // 신뢰할 수 있는 I/O만 사용해 시그널 핸들러 안에서도 crash 로그를 남긴다.
    // 동적 할당이나 복잡한 라이브러리 호출은 2차 크래시를 부를 수 있으므로 피한다.
    if (!text) return;
    std::size_t len = 0;
    while (text[len] != '\0') ++len;
#if defined(_WIN32)
    ::OutputDebugStringA(text);
    HANDLE stderr_handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (stderr_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(stderr_handle, text, static_cast<DWORD>(len), &written, nullptr);
    }
#else
    ::write(STDERR_FILENO, text, len);
#endif
}

void safe_write_newline() noexcept {
    safe_write_literal("\n");
}

void safe_write_number(unsigned long long value) noexcept {
    char buffer[32];
    std::size_t pos = 0;
    // `snprintf` 같은 런타임 의존을 피하고 10진수 문자열을 수동으로 만든다.
    // crash 처리 경로에서는 보기 좋은 포맷보다 "추가 실패 없이 남기는 것"이 더 중요하다.
    do {
        buffer[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && pos < sizeof(buffer));
    for (std::size_t i = 0; i < pos / 2; ++i) {
        std::swap(buffer[i], buffer[pos - i - 1]);
    }
    buffer[pos] = '\0';
    safe_write_literal(buffer);
}

#if defined(_WIN32)
void dump_stack() noexcept {
    // Windows에서는 DbgHelp API를 사용해 간단한 stack trace를 stderr로 복사한다.
    // 최소한의 심볼 정보만 남기고, 더 복잡한 formatter는 피한다.
    void* frames[64];
    USHORT captured = ::RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    HANDLE process = ::GetCurrentProcess();

    char symbol_buffer[sizeof(SYMBOL_INFO) + 256];
    PSYMBOL_INFO symbol = reinterpret_cast<PSYMBOL_INFO>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;

    for (USHORT i = 0; i < captured; ++i) {
        DWORD64 displacement = 0;
        if (::SymFromAddr(process, reinterpret_cast<DWORD64>(frames[i]), &displacement, symbol)) {
            safe_write_literal("#");
            safe_write_number(i);
            safe_write_literal(" ");
            safe_write_literal(symbol->Name);
            safe_write_literal(" +0x");

            char hex_buffer[17];
            const char* digits = "0123456789abcdef";
            DWORD64 value = displacement;
            std::size_t idx = 0;
            do {
                hex_buffer[idx++] = digits[value & 0xF];
                value >>= 4;
            } while (value && idx < sizeof(hex_buffer));
            for (std::size_t j = 0; j < idx / 2; ++j) {
                std::swap(hex_buffer[j], hex_buffer[idx - j - 1]);
            }
            hex_buffer[idx] = '\0';
            safe_write_literal(hex_buffer);
            safe_write_newline();
        }
    }
}
#else
void dump_stack() noexcept {
    // POSIX 계열은 `backtrace(3)`를 그대로 사용해 파일 디스크립터에 전송한다.
    // signal-safe에 가까운 단순 경로를 우선한다.
    void* buffer[64];
    int size = ::backtrace(buffer, 64);
    if (size > 0) {
        ::backtrace_symbols_fd(buffer, size, STDERR_FILENO);
    }
}
#endif

void handle_crash_signal(int signo) noexcept {
    safe_write_literal("=== Dynaxis crash handler: signal ");
    safe_write_number(static_cast<unsigned long long>(signo));
    safe_write_literal(" ===");
    safe_write_newline();
    dump_stack();
    safe_write_literal("=== crash handler end ===");
    safe_write_newline();
    std::signal(signo, SIG_DFL);
    std::raise(signo);
}

#if defined(_WIN32)
LONG WINAPI unhandled_exception_handler(EXCEPTION_POINTERS* info) {
    (void)info;
    safe_write_literal("=== Dynaxis crash handler: unhandled exception ===\n");
    dump_stack();
    safe_write_literal("=== crash handler end ===\n");
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#if !defined(_WIN32)
void install_handlers() {
    struct sigaction sa {};
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handle_crash_signal;

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
#endif

} // namespace

void install() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        // 여러 번 `install()`을 호출해도 OS 핸들러는 한 번만 세팅되도록 보장한다.
        // 초기화 경로가 여러 군데여도 crash hook 중복 등록으로 상태가 꼬이지 않게 하기 위함이다.
        return;
    }

#if defined(_WIN32)
    ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
    ::SetUnhandledExceptionFilter(unhandled_exception_handler);
#endif

#if defined(_WIN32)
    std::signal(SIGSEGV, handle_crash_signal);
    std::signal(SIGILL, handle_crash_signal);
    std::signal(SIGABRT, handle_crash_signal);
    std::signal(SIGFPE, handle_crash_signal);
#  ifdef SIGBUS
    std::signal(SIGBUS, handle_crash_signal);
#  endif
#else
    install_handlers();
#endif

    // `main()` 시작 시점에 호출하면 예외와 시그널을 모두 같은 로그 경로로 수집할 수 있다.
    // 장애가 난 뒤에 설치하는 것은 의미가 없으므로 부트스트랩 초반 고정이 중요하다.
    server::core::log::info("CrashHandler installed.");
}

} // namespace server::core::util::crash
