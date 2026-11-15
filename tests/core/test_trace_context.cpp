#include <gtest/gtest.h>

#include <server/core/trace/context.hpp>
#include <server/core/util/log.hpp>

#include <cstdlib>
#include <string>
#include <optional>
#include <vector>

namespace {

void set_env_value(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value ? value : "");
#else
    if (value) { setenv(key, value, 1); } else { unsetenv(key); }
#endif
}

} // namespace

TEST(TraceContextTest, InjectsTraceAndCorrelationIntoLogsWhenEnabled) {
    struct LogFormatScope {
        std::optional<std::string> previous;
        LogFormatScope() {
            if (const char* value = std::getenv("LOG_FORMAT")) { previous = value; }
            set_env_value("LOG_FORMAT", "text");
        }
        ~LogFormatScope() { set_env_value("LOG_FORMAT", previous ? previous->c_str() : nullptr); }
    } log_format;
    set_env_value("RUNTIME_TRACING_ENABLED", "1");
    set_env_value("RUNTIME_TRACING_SAMPLE_PERCENT", "100");
    server::core::trace::reset_for_tests();

    EXPECT_TRUE(server::core::trace::enabled());
    EXPECT_EQ(server::core::trace::sample_percent(), 100u);
    EXPECT_TRUE(server::core::trace::should_sample(7));

    const std::string trace_id = server::core::trace::make_trace_id();
    const std::string correlation_id = "trace-context-test-correlation";
    server::core::trace::ScopedContext scope(trace_id, correlation_id, true);
    ASSERT_TRUE(scope.active());

    server::core::log::set_level(server::core::log::level::debug);
    server::core::log::set_buffer_capacity(64);
    server::core::log::info("trace-context-test-marker");

    const std::vector<std::string> lines = server::core::log::recent(64);
    bool found_marker = false;
    for (const auto& line : lines) {
        if (line.find("trace-context-test-marker") == std::string::npos) {
            continue;
        }
        found_marker = true;
        EXPECT_NE(line.find("trace_id=" + trace_id), std::string::npos);
        EXPECT_NE(line.find("correlation_id=" + correlation_id), std::string::npos);
        break;
    }
    EXPECT_TRUE(found_marker);

    set_env_value("RUNTIME_TRACING_ENABLED", "0");
    server::core::trace::reset_for_tests();
}
