#include "server/core/scripting/lua_runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace server::core::scripting {
namespace {

class LuaRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto nonce = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir_ = std::filesystem::temp_directory_path()
            / ("knights_lua_runtime_test_" + nonce);
        ASSERT_TRUE(std::filesystem::create_directories(temp_dir_));
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    static void write_text(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << text;
        out.flush();
        ASSERT_TRUE(out.good());
    }

    std::filesystem::path temp_dir_;
};

TEST_F(LuaRuntimeTest, LoadScriptReflectsBuildToggle) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "sample.lua";
    write_text(script_path, "return 1\n");

    const auto result = runtime.load_script(script_path, "sample");

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.error.empty());

    const auto metrics = runtime.metrics_snapshot();
    EXPECT_EQ(metrics.loaded_scripts, 1u);
#else
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
    EXPECT_NE(result.error.find("disabled"), std::string::npos);
#endif
}

TEST_F(LuaRuntimeTest, CallUnknownEnvironmentIsSkippableWhenEnabled) {
    LuaRuntime runtime;

    const auto result = runtime.call("missing_env", "on_login");

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.executed);
    EXPECT_TRUE(result.error.empty());
#else
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.executed);
    EXPECT_NE(result.error.find("disabled"), std::string::npos);
#endif
}

TEST_F(LuaRuntimeTest, CallLoadedEnvironmentIsSkippableInScaffoldMode) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "sample.lua";
    write_text(script_path, "return 1\n");
    (void)runtime.load_script(script_path, "sample");

    const auto result = runtime.call("sample", "on_login");

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.executed);
    EXPECT_TRUE(result.error.empty());
#else
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.executed);
    EXPECT_NE(result.error.find("disabled"), std::string::npos);
#endif
}

TEST_F(LuaRuntimeTest, ReloadScriptsReplacesTrackedEnvironmentSet) {
    LuaRuntime runtime;

    const auto first_script = temp_dir_ / "first.lua";
    const auto second_script = temp_dir_ / "second.lua";
    write_text(first_script, "return 1\n");
    write_text(second_script, "return 2\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{first_script, "env_first"});

    const auto first_reload = runtime.reload_scripts(scripts);

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(first_reload.error.empty());
    EXPECT_EQ(first_reload.loaded, 1u);
    EXPECT_EQ(first_reload.failed, 0u);
    EXPECT_EQ(runtime.metrics_snapshot().loaded_scripts, 1u);

    scripts.clear();
    scripts.push_back(LuaRuntime::ScriptEntry{second_script, "env_second"});

    const auto second_reload = runtime.reload_scripts(scripts);
    EXPECT_TRUE(second_reload.error.empty());
    EXPECT_EQ(second_reload.loaded, 1u);
    EXPECT_EQ(second_reload.failed, 0u);
    EXPECT_EQ(runtime.metrics_snapshot().loaded_scripts, 1u);

    const auto old_env = runtime.call("env_first", "on_login");
    EXPECT_TRUE(old_env.ok);
    EXPECT_FALSE(old_env.executed);
#else
    EXPECT_FALSE(first_reload.error.empty());
    EXPECT_EQ(first_reload.loaded, 0u);
    EXPECT_EQ(first_reload.failed, 1u);
#endif
}

TEST_F(LuaRuntimeTest, CallAllTracksLoadedScriptCountInScaffoldMode) {
    LuaRuntime runtime;

    const auto first_script = temp_dir_ / "first.lua";
    const auto second_script = temp_dir_ / "second.lua";
    write_text(first_script, "return 1\n");
    write_text(second_script, "return 2\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{first_script, "env_first"});
    scripts.push_back(LuaRuntime::ScriptEntry{second_script, "env_second"});

    const auto reload = runtime.reload_scripts(scripts);
    const auto before = runtime.metrics_snapshot();
    const auto result = runtime.call_all("on_login");

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.attempted, 2u);
    EXPECT_EQ(result.failed, 0u);
    EXPECT_EQ(result.decision, LuaHookDecision::kPass);
    EXPECT_TRUE(result.error.empty());

    const auto after = runtime.metrics_snapshot();
    EXPECT_EQ(after.calls_total, before.calls_total + 2u);
#else
    EXPECT_FALSE(reload.error.empty());
    EXPECT_FALSE(result.error.empty());
#endif
}

TEST_F(LuaRuntimeTest, CallAllParsesReturnTableDecisionForMatchingHook) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "policy.lua";
    write_text(script_path,
               "return { hook = \"on_login\", decision = \"deny\", reason = \"login denied by lua scaffold\" }\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload = runtime.reload_scripts(scripts);

    const auto login_result = runtime.call_all("on_login");
    const auto join_result = runtime.call_all("on_join");

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(reload.error.empty());

    EXPECT_EQ(login_result.decision, LuaHookDecision::kDeny);
    EXPECT_EQ(login_result.reason, "login denied by lua scaffold");
    EXPECT_TRUE(login_result.error.empty());

    EXPECT_EQ(join_result.decision, LuaHookDecision::kPass);
    EXPECT_TRUE(join_result.reason.empty());
    EXPECT_TRUE(join_result.error.empty());
#else
    EXPECT_FALSE(reload.error.empty());
    EXPECT_FALSE(login_result.error.empty());
    EXPECT_FALSE(join_result.error.empty());
#endif
}

TEST_F(LuaRuntimeTest, CallAllIgnoresInvalidDirectiveForDifferentHook) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "policy.lua";
    write_text(script_path,
               "-- hook=on_join decision=denyy reason=invalid token for join\n"
               "return 1\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload = runtime.reload_scripts(scripts);
    const auto result = runtime.call_all("on_login");

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.decision, LuaHookDecision::kPass);
    EXPECT_TRUE(result.reason.empty());
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.failed, 0u);
#else
    EXPECT_FALSE(reload.error.empty());
    EXPECT_FALSE(result.error.empty());
#endif
}

TEST_F(LuaRuntimeTest, CallAllUsesDeterministicReasonAcrossMultipleDenyScripts) {
    LuaRuntime runtime;

    const auto script_a = temp_dir_ / "a.lua";
    const auto script_b = temp_dir_ / "b.lua";
    write_text(script_a,
               "return { hook = \"on_login\", decision = \"deny\", reason = \"reason from env_a\" }\n");
    write_text(script_b,
               "return { hook = \"on_login\", decision = \"deny\", reason = \"reason from env_b\" }\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_b, "env_b"});
    scripts.push_back(LuaRuntime::ScriptEntry{script_a, "env_a"});

    const auto reload = runtime.reload_scripts(scripts);
    const auto result = runtime.call_all("on_login");

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.decision, LuaHookDecision::kDeny);
    EXPECT_EQ(result.reason, "reason from env_a");
    EXPECT_TRUE(result.error.empty());
#else
    EXPECT_FALSE(reload.error.empty());
    EXPECT_FALSE(result.error.empty());
#endif
}

TEST_F(LuaRuntimeTest, ResetClearsRuntimeState) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "sample.lua";
    write_text(script_path, "return 1\n");

    (void)runtime.register_host_api("server", "log_info", []() {});
    (void)runtime.load_script(script_path, "sample");
    (void)runtime.call("sample", "on_login");

    runtime.reset();
    const auto metrics = runtime.metrics_snapshot();

    EXPECT_EQ(metrics.loaded_scripts, 0u);
    EXPECT_EQ(metrics.registered_host_api, 0u);
    EXPECT_EQ(metrics.calls_total, 0u);
}

} // namespace
} // namespace server::core::scripting
