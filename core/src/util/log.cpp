#include "server/core/util/log.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace server::core::log {

namespace {
std::atomic<level> g_level{level::info};
std::mutex g_mu;

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

void emit(level lv, const std::string& msg) {
    if (static_cast<int>(lv) < static_cast<int>(g_level.load())) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T");
    std::cerr << oss.str() << " [" << to_cstr(lv) << "] " << msg << std::endl;
}
}

void set_level(level lv) { g_level.store(lv); }
void trace(const std::string& msg) { emit(level::trace, msg); }
void debug(const std::string& msg) { emit(level::debug, msg); }
void info(const std::string& msg)  { emit(level::info, msg); }
void warn(const std::string& msg)  { emit(level::warn, msg); }
void error(const std::string& msg) { emit(level::error, msg); }

} // namespace server::core::log

