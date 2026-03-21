#include "server/core/util/log.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/trace/context.hpp"

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
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

level parse_level_from_env() {
    if (const char* raw = std::getenv("LOG_LEVEL"); raw && *raw) {
        std::string value(raw);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (value == "trace") {
            return level::trace;
        }
        if (value == "debug") {
            return level::debug;
        }
        if (value == "warn" || value == "warning") {
            return level::warn;
        }
        if (value == "error") {
            return level::error;
        }
    }
    return level::info;
}

std::size_t parse_buffer_capacity_from_env() {
    constexpr std::size_t kDefaultCapacity = 256;
    if (const char* raw = std::getenv("LOG_BUFFER_CAPACITY"); raw && *raw) {
        try {
            const auto parsed = std::stoull(raw);
            if (parsed > 0 && parsed <= 1'000'000) {
                return static_cast<std::size_t>(parsed);
            }
        } catch (...) {
        }
    }
    return kDefaultCapacity;
}

// 현재 로그 레벨을 저장하는 원자적 변수.
// fast path에서 락 없이 필터링할 수 있어야 request 경로가 로그 설정 조회 때문에 막히지 않는다.
std::atomic<level> g_level{parse_level_from_env()};

// 최근 로그를 저장하는 버퍼.
// recent buffer는 `/logs`와 현장 디버깅에서 "방금 무슨 일이 있었는가"를 빠르게 확인하는 용도다.
std::deque<std::string> g_buffer;
std::size_t g_buffer_capacity = parse_buffer_capacity_from_env();
std::mutex g_buffer_mu; // g_buffer 보호용 뮤텍스

// 비동기 로깅 파이프라인.
// 메인 스레드는 큐 삽입만 하고, 실제 stderr 출력은 백그라운드 스레드가 수행한다.
// 그렇지 않으면 로그 flush 지연이 곧바로 request path tail latency로 번질 수 있다.
class AsyncLogger {
public:
    // 로그 메시지를 큐에 넣고 워커 스레드를 깨운다.
    // 이 함수는 메인 로직 스레드에서 호출되므로 최대한 빨리 복귀해야 한다.
    // 값 전달을 받아 호출자가 rvalue를 넘기면 큐 삽입 시 move 경로를 그대로 활용한다.
    void push(std::string msg) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (capacity_ > 0) {
                if (overflow_policy_ == OverflowPolicy::kBlock) {
                    cv_space_.wait(lock, [this] {
                        return stop_ || queue_.size() < capacity_;
                    });
                    if (stop_) {
                        return;
                    }
                } else if (queue_.size() >= capacity_) {
                    // overflow 정책을 명시적으로 두는 이유는, 로그 폭주 시 어떤 식으로 degrade할지
                    // 운영자가 선택할 수 있어야 하기 때문이다. 무제한 큐는 결국 더 큰 장애를 만든다.
                    if (overflow_policy_ == OverflowPolicy::kDropOldest) {
                        queue_.pop();
                    } else {
                        server::core::runtime_metrics::record_log_async_queue_drop();
                        server::core::runtime_metrics::record_log_async_queue_depth(queue_.size());
                        return;
                    }
                    server::core::runtime_metrics::record_log_async_queue_drop();
                }
            }

            queue_.push(std::move(msg));
            server::core::runtime_metrics::record_log_async_queue_depth(queue_.size());
        }
        cv_.notify_one();
    }

private:
    enum class OverflowPolicy {
        kDropNewest,
        kDropOldest,
        kBlock,
    };

    static std::size_t parse_queue_capacity() {
        constexpr std::size_t kDefaultCapacity = 8192;
        if (const char* raw = std::getenv("LOG_ASYNC_QUEUE_CAPACITY"); raw && *raw) {
            try {
                const auto parsed = std::stoull(raw);
                if (parsed > 0) {
                    return static_cast<std::size_t>(parsed);
                }
            } catch (...) {
            }
        }
        return kDefaultCapacity;
    }

    static OverflowPolicy parse_overflow_policy() {
        if (const char* raw = std::getenv("LOG_ASYNC_QUEUE_OVERFLOW"); raw && *raw) {
            std::string policy(raw);
            std::transform(policy.begin(), policy.end(), policy.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (policy == "drop_oldest" || policy == "oldest") {
                return OverflowPolicy::kDropOldest;
            }
            if (policy == "block") {
                return OverflowPolicy::kBlock;
            }
        }
        return OverflowPolicy::kDropNewest;
    }

    // 생성자를 private으로 두어 단일 비동기 로깅 파이프라인만 유지한다.
    AsyncLogger()
        : capacity_(parse_queue_capacity())
        , overflow_policy_(parse_overflow_policy())
        , stop_(false) {
        server::core::runtime_metrics::register_log_async_queue_capacity(capacity_);
        // 백그라운드 워커 스레드를 시작한다.
        worker_ = std::thread([this] { worker_loop(); });
    }

    // 소멸자에서 워커 스레드가 안전하게 종료되도록 대기한다.
    ~AsyncLogger() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true; // 스레드 종료 플래그 설정
        }
        cv_.notify_one(); // 대기 중인 스레드를 깨운다.
        cv_space_.notify_all();
        if (worker_.joinable()) {
            worker_.join(); // 스레드가 종료될 때까지 기다린다.
        }
    }

    // 워커 스레드의 메인 루프.
    // 실제 I/O는 여기서만 일어나므로, 생산자 스레드는 stderr 속도에 직접 묶이지 않는다.
    void worker_loop() {
        while (true) {
            std::string msg;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 큐가 비어있거나 종료 플래그가 설정될 때까지 대기한다.
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });

                // 종료 플래그가 설정되었고 큐가 비어있으면 스레드를 종료한다.
                if (stop_ && queue_.empty()) {
                    return;
                }

                // 큐에 메시지가 있으면 가져온다.
                if (!queue_.empty()) {
                    msg = std::move(queue_.front());
                    queue_.pop();
                    server::core::runtime_metrics::record_log_async_queue_depth(queue_.size());
                    cv_space_.notify_one();
                }
            }

            // 실제 출력은 락을 푼 상태에서 수행해 생산자 스레드의 대기를 최소화한다.
            if (!msg.empty()) {
                const auto started_at = std::chrono::steady_clock::now();
                std::cerr << msg << std::endl; // 표준 에러 스트림으로 출력
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started_at);
                server::core::runtime_metrics::record_log_async_flush_latency(elapsed);
            }
        }
    }

    std::mutex mutex_; // 큐 접근용 뮤텍스
    std::condition_variable cv_; // 큐 상태 변경을 알리는 조건 변수
    std::condition_variable cv_space_;
    std::queue<std::string> queue_; // 로그 메시지를 저장하는 큐
    std::size_t capacity_{0};
    OverflowPolicy overflow_policy_{OverflowPolicy::kDropNewest};
    std::thread worker_; // 로그 처리를 담당하는 백그라운드 스레드
    bool stop_; // 스레드 종료를 위한 플래그

    // `get_logger()`만 이 인스턴스를 소유하도록 friend로 연다.
    friend AsyncLogger& get_logger();
};

// 정적 로컬 변수 기반 싱글톤 생성.
// C++11 이후 정적 지역 변수 초기화는 스레드 안전하므로 별도 초기화 락이 필요 없다.
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

bool log_format_json() {
    if (const char* raw = std::getenv("LOG_FORMAT"); raw && *raw) {
        std::string value(raw);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value == "json";
    }
    return false;
}

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const unsigned char ch : input) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                std::ostringstream hex;
                hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                out += hex.str();
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::string extract_token_value(std::string_view message, std::string_view key) {
    const std::string token = std::string(key) + "=";
    const auto pos = message.find(token);
    if (pos == std::string_view::npos) {
        return {};
    }
    std::size_t begin = pos + token.size();
    std::size_t end = begin;
    while (end < message.size()) {
        const char c = message[end];
        if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == ',' || c == ';') {
            break;
        }
        ++end;
    }
    return std::string(message.substr(begin, end - begin));
}

bool looks_like_json_object(std::string_view line) {
    return line.size() >= 2 && line.front() == '{' && line.back() == '}';
}

void record_log_schema_metrics(std::string_view format,
                               bool parse_success,
                               std::string_view timestamp,
                               std::string_view level_name,
                               std::string_view component,
                               std::string_view trace_id,
                               std::string_view correlation_id,
                               std::string_view message,
                               std::string_view error_code) {
    try {
        const std::string format_label(format);

        auto& records = server::core::metrics::get_counter("core_log_schema_records_total");
        records.inc(1.0, {{"format", format_label}});

        auto& parse_metric = server::core::metrics::get_counter(
            parse_success ? "core_log_schema_parse_success_total" : "core_log_schema_parse_failure_total");
        parse_metric.inc(1.0, {{"format", format_label}});

        auto& field_total = server::core::metrics::get_counter("core_log_schema_field_total");
        auto& field_filled = server::core::metrics::get_counter("core_log_schema_field_filled_total");

        const auto mark_field = [&](std::string_view field, std::string_view value) {
            const std::string field_label(field);
            field_total.inc(1.0, {{"field", field_label}, {"format", format_label}});
            if (!value.empty()) {
                field_filled.inc(1.0, {{"field", field_label}, {"format", format_label}});
            }
        };

        mark_field("timestamp", timestamp);
        mark_field("level", level_name);
        mark_field("component", component);
        mark_field("trace_id", trace_id);
        mark_field("correlation_id", correlation_id);
        mark_field("message", message);
        mark_field("error_code", error_code);
    } catch (...) {
        server::core::runtime_metrics::record_exception_ignored();
    }
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
    std::ostringstream ts;
    ts << std::put_time(&tm, "%F %T");
    const std::string timestamp = ts.str();

    const auto trace_id = trace::current_sampled() ? trace::current_trace_id() : std::string();
    const auto correlation_id = trace::current_sampled() ? trace::current_correlation_id() : std::string();
    const std::string component = [&]() {
        std::string value = extract_token_value(msg, "component");
        if (value.empty()) {
            value = "core";
        }
        return value;
    }();
    const std::string error_code = extract_token_value(msg, "error_code");

    if (log_format_json()) {
        std::ostringstream oss;
        oss << "{";
        oss << "\"timestamp\":\"" << json_escape(timestamp) << "\",";
        oss << "\"level\":\"" << to_cstr(lv) << "\",";
        oss << "\"component\":\"" << json_escape(component) << "\",";
        oss << "\"trace_id\":\"" << json_escape(trace_id) << "\",";
        oss << "\"correlation_id\":\"" << json_escape(correlation_id) << "\",";
        oss << "\"error_code\":\"" << json_escape(error_code) << "\",";
        oss << "\"message\":\"" << json_escape(msg) << "\"";
        oss << "}";
        std::string line = oss.str();
        record_log_schema_metrics("json",
                                  looks_like_json_object(line),
                                  timestamp,
                                  to_cstr(lv),
                                  component,
                                  trace_id,
                                  correlation_id,
                                  msg,
                                  error_code);
        return line;
    }

    std::ostringstream oss;
    oss << timestamp << " [" << to_cstr(lv) << "] " << msg;

    if (!trace_id.empty() || !correlation_id.empty()) {
        if (!trace_id.empty()) {
            oss << " trace_id=" << trace_id;
        }
        if (!correlation_id.empty()) {
            oss << " correlation_id=" << correlation_id;
        }
    }

    std::string line = oss.str();
    record_log_schema_metrics("text",
                              !line.empty(),
                              timestamp,
                              to_cstr(lv),
                              component,
                              trace_id,
                              correlation_id,
                              msg,
                              error_code);
    return line;
}

std::string mask_sensitive_fields(const std::string& raw, std::uint64_t& masked_fields) {
    std::string out = raw;
    std::string lower = out;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const auto mask_after = [&](std::string_view token) {
        std::size_t pos = 0;
        while (true) {
            pos = lower.find(token, pos);
            if (pos == std::string::npos) {
                break;
            }

            std::size_t value_begin = pos + token.size();
            while (value_begin < out.size() && std::isspace(static_cast<unsigned char>(out[value_begin])) != 0) {
                ++value_begin;
            }
            std::size_t value_end = value_begin;
            while (value_end < out.size()) {
                const char c = out[value_end];
                if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == '&' || c == ',' || c == '"' || c == '\'' || c == ';') {
                    break;
                }
                ++value_end;
            }
            if (value_end > value_begin) {
                out.replace(value_begin, value_end - value_begin, "***");
                lower = out;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                ++masked_fields;
                pos = value_begin + 3;
            } else {
                pos += token.size();
            }
        }
    };

    mask_after("authorization: bearer ");
    mask_after("token=");
    mask_after("password=");
    mask_after("secret=");
    mask_after("api_key=");
    return out;
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

    std::uint64_t masked_fields = 0;
    const std::string sanitized = mask_sensitive_fields(msg, masked_fields);
    if (masked_fields > 0) {
        server::core::runtime_metrics::record_log_masked_fields(masked_fields);
    }
    std::string line = format_line(lv, sanitized);
    
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
