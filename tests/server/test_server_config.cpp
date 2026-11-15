#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include <server/app/config.hpp>

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string key, const char* value)
        : key_(std::move(key)) {
        if (const char* old = std::getenv(key_.c_str()); old != nullptr) {
            had_old_ = true;
            old_value_ = old;
        }
        set(value);
    }

    ~ScopedEnvVar() {
#if defined(_WIN32)
        if (had_old_) {
            _putenv_s(key_.c_str(), old_value_.c_str());
        } else {
            _putenv_s(key_.c_str(), "");
        }
#else
        if (had_old_) {
            setenv(key_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(key_.c_str());
        }
#endif
    }

private:
    void set(const char* value) const {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), value ? value : "");
#else
        if (value) {
            setenv(key_.c_str(), value, 1);
        } else {
            unsetenv(key_.c_str());
        }
#endif
    }

    std::string key_;
    bool had_old_{false};
    std::string old_value_;
};

} // namespace







TEST(ServerConfigTest, DrainAndMetricsConfigurationRemainAvailable) {
    server::app::ServerConfig config;
    EXPECT_GT(config.shutdown_drain_timeout_ms, 0u);
    EXPECT_GT(config.shutdown_drain_poll_ms, 0u);
}
