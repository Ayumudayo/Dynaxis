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

TEST(ServerConfigTest, LuaDefaultsAppliedWhenEnvironmentIsUnset) {
    ScopedEnvVar enabled("LUA_ENABLED", nullptr);
    ScopedEnvVar scripts_dir("LUA_SCRIPTS_DIR", nullptr);
    ScopedEnvVar lock_path("LUA_LOCK_PATH", nullptr);
    ScopedEnvVar reload_ms("LUA_RELOAD_INTERVAL_MS", nullptr);
    ScopedEnvVar instruction_limit("LUA_INSTRUCTION_LIMIT", nullptr);
    ScopedEnvVar memory_limit("LUA_MEMORY_LIMIT_BYTES", nullptr);
    ScopedEnvVar auto_disable("LUA_AUTO_DISABLE_THRESHOLD", nullptr);

    server::app::ServerConfig config;
    char app_name[] = "server_app";
    char* argv[] = {app_name};

    ASSERT_TRUE(config.load(1, argv));
    EXPECT_FALSE(config.lua_enabled);
    EXPECT_TRUE(config.lua_scripts_dir.empty());
    EXPECT_TRUE(config.lua_lock_path.empty());
    EXPECT_EQ(config.lua_reload_interval_ms, 1000u);
    EXPECT_EQ(config.lua_instruction_limit, 100000u);
    EXPECT_EQ(config.lua_memory_limit_bytes, 1048576u);
    EXPECT_EQ(config.lua_auto_disable_threshold, 3u);
}

TEST(ServerConfigTest, LuaEnvironmentOverridesAreParsed) {
    ScopedEnvVar enabled("LUA_ENABLED", "1");
    ScopedEnvVar scripts_dir("LUA_SCRIPTS_DIR", "/app/scripts");
    ScopedEnvVar lock_path("LUA_LOCK_PATH", "/app/scripts/.reload.lock");
    ScopedEnvVar reload_ms("LUA_RELOAD_INTERVAL_MS", "2500");
    ScopedEnvVar instruction_limit("LUA_INSTRUCTION_LIMIT", "200000");
    ScopedEnvVar memory_limit("LUA_MEMORY_LIMIT_BYTES", "2097152");
    ScopedEnvVar auto_disable("LUA_AUTO_DISABLE_THRESHOLD", "5");

    server::app::ServerConfig config;
    char app_name[] = "server_app";
    char* argv[] = {app_name};

    ASSERT_TRUE(config.load(1, argv));
    EXPECT_TRUE(config.lua_enabled);
    EXPECT_EQ(config.lua_scripts_dir, "/app/scripts");
    EXPECT_EQ(config.lua_lock_path, "/app/scripts/.reload.lock");
    EXPECT_EQ(config.lua_reload_interval_ms, 2500u);
    EXPECT_EQ(config.lua_instruction_limit, 200000u);
    EXPECT_EQ(config.lua_memory_limit_bytes, 2097152u);
    EXPECT_EQ(config.lua_auto_disable_threshold, 5u);
}
