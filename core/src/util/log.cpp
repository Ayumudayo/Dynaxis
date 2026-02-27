#include "server/core/util/log.hpp"
#include "server/core/trace/context.hpp"

#include <atomic>
#include <cstddef>
#include <chrono>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>
#include <thread>
#include <condition_variable>
#include <queue>

/**
 * @brief 비동기 로그 버퍼/출력 파이프라인 구현입니다.
 *
 * 요청 처리 스레드는 로그를 큐에 넣고 즉시 복귀하고,
 * 백그라운드 워커가 실제 출력과 버퍼 관리를 담당해 경로 지연을 낮춥니다.
 */
namespace server::core::log {

namespace {

// 로그 레벨을 저장하는 원자적 변수 (기본값: INFO)
std::atomic<level> g_level{level::info};

// 최근 로그를 저장하는 버퍼 (디버깅/모니터링 용도)
std::deque<std::string> g_buffer;
std::size_t g_buffer_capacity = 256;
std::mutex g_buffer_mu; // g_buffer 보호용 뮤텍스

// 비동기 로깅을 위한 구조체
// 이 클래스는 백그라운드 스레드에서 로그 메시지를 처리하여
// 메인 스레드의 블로킹을 최소화합니다.
// 즉, 로그를 남기느라 서버 성능이 저하되는 것을 방지합니다.
class AsyncLogger {
public:
    // 로그 메시지를 큐에 넣고 워커 스레드를 깨웁니다.
    // 이 함수는 메인 로직 스레드에서 호출되므로 최대한 빨리 리턴해야 합니다.
    // 값 전달을 받아 호출자가 rvalue를 넘기면 큐 삽입 시 move 경로를 그대로 활용합니다.
    void push(std::string msg) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(msg));
        }
        cv_.notify_one();
    }

private:
    // 생성자는 private으로 선언하여 싱글톤 패턴을 강제합니다.
    AsyncLogger() : stop_(false) {
        // 백그라운드 워커 스레드를 시작합니다.
        worker_ = std::thread([this] { worker_loop(); });
    }

    // 소멸자에서 워커 스레드가 안전하게 종료되도록 대기합니다.
    ~AsyncLogger() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true; // 스레드 종료 플래그 설정
        }
        cv_.notify_one(); // 대기 중인 스레드를 깨웁니다.
        if (worker_.joinable()) {
            worker_.join(); // 스레드가 종료될 때까지 기다립니다.
        }
    }

    // 워커 스레드에서 실행될 메인 루프입니다.
    void worker_loop() {
        while (true) {
            std::string msg;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 큐가 비어있거나 종료 플래그가 설정될 때까지 대기합니다.
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });

                // 종료 플래그가 설정되었고 큐가 비어있으면 스레드를 종료합니다.
                if (stop_ && queue_.empty()) {
                    return;
                }

                // 큐에 메시지가 있으면 가져옵니다.
                if (!queue_.empty()) {
                    msg = std::move(queue_.front());
                    queue_.pop();
                }
            }

            // 실제 출력은 락을 푼 상태에서 수행하여 생산자 스레드의 대기를 최소화합니다.
            if (!msg.empty()) {
                std::cerr << msg << std::endl; // 표준 에러 스트림으로 출력
            }
        }
    }

    std::mutex mutex_; // 큐 접근을 위한 뮤텍스
    std::condition_variable cv_; // 큐 상태 변경을 알리는 조건 변수
    std::queue<std::string> queue_; // 로그 메시지를 저장하는 큐
    std::thread worker_; // 로그 처리를 담당하는 백그라운드 스레드
    bool stop_; // 스레드 종료를 위한 플래그

    // AsyncLogger 인스턴스를 얻기 위한 friend 함수 선언
    friend AsyncLogger& get_logger();
};

// 정적 로컬 변수를 사용하여 싱글톤 인스턴스를 생성합니다.
// C++11부터는 스레드 안전성이 보장됩니다.
AsyncLogger& get_logger() {
    static AsyncLogger logger;
    return logger;
}

const char* to_cstr(level lv) {
    switch (lv) {
    case level::trace: return "TRACE";
    case level::debug: return "DEBUG";
    case level::info:  return "INFO";
    case level::warn:  return "WARN";
    case level::error: return "ERROR";
    }
    return "INFO";
}

std::string format_line(level lv, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T") << " [" << to_cstr(lv) << "] " << msg;

    if (trace::current_sampled()) {
        const auto trace_id = trace::current_trace_id();
        const auto correlation_id = trace::current_correlation_id();
        if (!trace_id.empty()) {
            oss << " trace_id=" << trace_id;
        }
        if (!correlation_id.empty()) {
            oss << " correlation_id=" << correlation_id;
        }
    }

    return oss.str();
}

void push_buffer(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_buffer_mu);
    if (g_buffer_capacity == 0) return;
    g_buffer.push_back(line);
    if (g_buffer.size() > g_buffer_capacity) {
        g_buffer.pop_front();
    }
}

void emit(level lv, const std::string& msg) {
    // 현재 로그 레벨보다 낮으면 무시
    if (static_cast<int>(lv) < static_cast<int>(g_level.load(std::memory_order_relaxed))) return;

    std::string line = format_line(lv, msg);
    
    // 1. 최근 로그 버퍼에 저장 (동기적, 뮤텍스 사용)
    push_buffer(line);

    // 2. 비동기 로거 큐에 삽입 (백그라운드 스레드가 출력)
    get_logger().push(std::move(line));
}

} // namespace

void set_level(level lv) { g_level.store(lv); }

void set_buffer_capacity(std::size_t capacity) {
    std::lock_guard<std::mutex> lk(g_buffer_mu);
    g_buffer_capacity = capacity;
    while (g_buffer.size() > g_buffer_capacity) {
        g_buffer.pop_front();
    }
}

std::vector<std::string> recent(std::size_t limit) {
    std::lock_guard<std::mutex> lk(g_buffer_mu);
    if (limit == 0 || limit > g_buffer.size()) {
        limit = g_buffer.size();
    }
    std::vector<std::string> snapshot;
    snapshot.reserve(limit);
    auto begin = g_buffer.end() - static_cast<std::ptrdiff_t>(limit);
    snapshot.insert(snapshot.end(), begin, g_buffer.end());
    return snapshot;
}

void trace(const std::string& msg) { emit(level::trace, msg); }
void debug(const std::string& msg) { emit(level::debug, msg); }
void info(const std::string& msg)  { emit(level::info, msg); }
void warn(const std::string& msg)  { emit(level::warn, msg); }
void error(const std::string& msg) { emit(level::error, msg); }

} // namespace server::core::log
