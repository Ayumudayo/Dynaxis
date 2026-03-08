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
            / ("lua_runtime_test_" + nonce);
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

TEST_F(LuaRuntimeTest, LoadScriptLoadsTrackedEnvironment) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "sample.lua";
    write_text(script_path, "return 1\n");

    const auto result = runtime.load_script(script_path, "sample");

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.error.empty());

    const auto metrics = runtime.metrics_snapshot();
    EXPECT_EQ(metrics.loaded_scripts, 1u);
}

TEST_F(LuaRuntimeTest, EnabledReturnsTrue) {
    LuaRuntime runtime;

    EXPECT_TRUE(runtime.enabled());
}

TEST_F(LuaRuntimeTest, CallUnknownEnvironmentIsSkippableWhenEnabled) {
    LuaRuntime runtime;

    const auto result = runtime.call("missing_env", "on_login");

    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.executed);
    EXPECT_TRUE(result.error.empty());
}

TEST_F(LuaRuntimeTest, CallLoadedEnvironmentIsSkippableInScaffoldMode) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "sample.lua";
    write_text(script_path, "return 1\n");
    (void)runtime.load_script(script_path, "sample");

    const auto result = runtime.call("sample", "on_login");

    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.executed);
    EXPECT_TRUE(result.error.empty());
}

TEST_F(LuaRuntimeTest, CallExecutesLuaFunctionWhenPresent) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "callable.lua";
    write_text(script_path,
               "function on_login()\n"
               "  return 1\n"
               "end\n");
    (void)runtime.load_script(script_path, "callable");

    const auto result = runtime.call("callable", "on_login");

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.executed);
    EXPECT_TRUE(result.error.empty());
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

    const auto epoch_before = runtime.metrics_snapshot().reload_epoch;
    EXPECT_TRUE(first_reload.error.empty());
    EXPECT_EQ(first_reload.loaded, 1u);
    EXPECT_EQ(first_reload.failed, 0u);
    EXPECT_EQ(runtime.metrics_snapshot().loaded_scripts, 1u);
    EXPECT_EQ(runtime.metrics_snapshot().reload_epoch, epoch_before);

    scripts.clear();
    scripts.push_back(LuaRuntime::ScriptEntry{second_script, "env_second"});

    const auto second_reload = runtime.reload_scripts(scripts);
    EXPECT_TRUE(second_reload.error.empty());
    EXPECT_EQ(second_reload.loaded, 1u);
    EXPECT_EQ(second_reload.failed, 0u);
    EXPECT_EQ(runtime.metrics_snapshot().loaded_scripts, 1u);
    EXPECT_EQ(runtime.metrics_snapshot().reload_epoch, epoch_before + 1u);

    const auto old_env = runtime.call("env_first", "on_login");
    EXPECT_TRUE(old_env.ok);
    EXPECT_FALSE(old_env.executed);
}

TEST_F(LuaRuntimeTest, ResetClearsReloadEpoch) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "sample.lua";
    write_text(script_path, "return 1\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "sample"});
    (void)runtime.reload_scripts(scripts);

    runtime.reset();

    EXPECT_EQ(runtime.metrics_snapshot().reload_epoch, 0u);
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

    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.attempted, 2u);
    EXPECT_EQ(result.failed, 0u);
    EXPECT_EQ(result.decision, LuaHookDecision::kPass);
    EXPECT_TRUE(result.error.empty());

    const auto after = runtime.metrics_snapshot();
    EXPECT_EQ(after.calls_total, before.calls_total + 2u);
}

TEST_F(LuaRuntimeTest, ErrorsAreTrackedPerApiCall) {
    LuaRuntime runtime;

    const auto before = runtime.metrics_snapshot();

    const auto script_path = temp_dir_ / "missing.lua";
    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "missing"});
    const auto reload = runtime.reload_scripts(scripts);

    const auto after_reload = runtime.metrics_snapshot();
    const auto call = runtime.call("missing", "on_login");
    const auto after_call = runtime.metrics_snapshot();
    const auto call_all = runtime.call_all("on_login");
    const auto after_call_all = runtime.metrics_snapshot();

    EXPECT_TRUE(reload.error.empty());
    EXPECT_TRUE(call.ok);
    EXPECT_TRUE(call.error.empty());
    EXPECT_TRUE(call_all.error.empty());
    EXPECT_EQ(after_reload.errors_total, before.errors_total + 1u);
    EXPECT_EQ(after_call.errors_total, after_reload.errors_total);
    EXPECT_EQ(after_call_all.errors_total, after_call.errors_total);
}

TEST_F(LuaRuntimeTest, CallAllParsesReturnTableDecisionForMatchingHook) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "policy.lua";
    write_text(script_path,
               "return { hook = \"on_login\", decision = \"deny\", reason = \"login denied by lua scaffold\", notice = \"welcome notice\" }\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload = runtime.reload_scripts(scripts);

    const auto login_result = runtime.call_all("on_login");
    const auto join_result = runtime.call_all("on_join");

    EXPECT_TRUE(reload.error.empty());

    EXPECT_EQ(login_result.decision, LuaHookDecision::kDeny);
    EXPECT_EQ(login_result.reason, "login denied by lua scaffold");
    ASSERT_EQ(login_result.notices.size(), 1u);
    EXPECT_EQ(login_result.notices.front(), "welcome notice");
    EXPECT_TRUE(login_result.error.empty());

    EXPECT_EQ(join_result.decision, LuaHookDecision::kPass);
    EXPECT_TRUE(join_result.reason.empty());
    EXPECT_TRUE(join_result.notices.empty());
    EXPECT_TRUE(join_result.error.empty());
}

TEST_F(LuaRuntimeTest, CallAllUsesFunctionReturnDecisionWhenHookExists) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "hook_table.lua";
    write_text(script_path,
               "function on_login()\n"
               "  return { decision = \"deny\", reason = \"deny from function\", notice = \"function notice\" }\n"
               "end\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "hook_table"});
    const auto reload = runtime.reload_scripts(scripts);
    const auto result = runtime.call_all("on_login");

    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.decision, LuaHookDecision::kDeny);
    EXPECT_EQ(result.reason, "deny from function");
    ASSERT_EQ(result.notices.size(), 1u);
    EXPECT_EQ(result.notices.front(), "function notice");
    EXPECT_TRUE(result.error.empty());
}

TEST_F(LuaRuntimeTest, CallAllPassesHookContextIntoLuaAndHostCallbacks) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "host_api.lua";
    write_text(script_path,
               "function on_login(ctx)\n"
               "  return { decision = \"deny\", reason = server.describe_user(ctx.user, ctx.session_id) }\n"
               "end\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "host_api"});
    const auto reload = runtime.reload_scripts(scripts);

    LuaRuntime::HostCallContext observed_context{};
    LuaRuntime::HostArgs observed_args;
    const bool registered = runtime.register_host_api(
        "server",
        "describe_user",
        [&observed_context, &observed_args](const LuaRuntime::HostArgs& args,
                                            const LuaRuntime::HostCallContext& context) {
            observed_args = args;
            observed_context = context;
            return LuaRuntime::HostCallResult{
                LuaRuntime::HostValue{"session " + std::to_string(context.hook.session_id) + " for " + context.hook.user},
                {}
            };
        });

    LuaHookContext ctx{};
    ctx.session_id = 77;
    ctx.user = "alice";
    const auto result = runtime.call_all("on_login", ctx);

    EXPECT_TRUE(reload.error.empty());
    EXPECT_TRUE(registered);
    EXPECT_EQ(result.decision, LuaHookDecision::kDeny);
    EXPECT_EQ(result.reason, "session 77 for alice");
    ASSERT_EQ(observed_args.size(), 2u);
    EXPECT_EQ(std::get<std::string>(observed_args[0].value), "alice");
    EXPECT_EQ(std::get<std::int64_t>(observed_args[1].value), 77);
    EXPECT_EQ(observed_context.hook_name, "on_login");
    EXPECT_EQ(observed_context.script_name, "host_api");
    EXPECT_EQ(observed_context.hook.user, "alice");
}

TEST_F(LuaRuntimeTest, CallAllAggregatesNoticesAcrossScripts) {
    LuaRuntime runtime;

    const auto script_a = temp_dir_ / "a.lua";
    const auto script_b = temp_dir_ / "b.lua";
    write_text(script_a,
               "return { hook = \"on_login\", decision = \"pass\", notice = \"notice from env_a\" }\n");
    write_text(script_b,
               "return { hook = \"on_login\", decision = \"pass\", notice = \"notice from env_b\" }\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_b, "env_b"});
    scripts.push_back(LuaRuntime::ScriptEntry{script_a, "env_a"});

    const auto reload = runtime.reload_scripts(scripts);
    const auto result = runtime.call_all("on_login");

    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.decision, LuaHookDecision::kPass);
    ASSERT_EQ(result.notices.size(), 2u);
    EXPECT_EQ(result.notices[0], "notice from env_a");
    EXPECT_EQ(result.notices[1], "notice from env_b");
    EXPECT_TRUE(result.error.empty());
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

    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.decision, LuaHookDecision::kPass);
    EXPECT_TRUE(result.reason.empty());
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.failed, 0u);
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

    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.decision, LuaHookDecision::kDeny);
    EXPECT_EQ(result.reason, "reason from env_a");
    EXPECT_TRUE(result.error.empty());
}

TEST_F(LuaRuntimeTest, CallAllInstructionLimitIncrementsLimitHits) {
    LuaRuntime::Config cfg{};
    cfg.instruction_limit = 1'000;
    LuaRuntime runtime(cfg);

    const auto script_path = temp_dir_ / "limit_instruction.lua";
    write_text(script_path,
               "function on_login(ctx)\n"
               "  while true do\n"
               "  end\n"
               "end\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "limit_instruction"});
    const auto reload = runtime.reload_scripts(scripts);

    const auto before = runtime.metrics_snapshot();
    const auto result = runtime.call_all("on_login");
    const auto after = runtime.metrics_snapshot();

    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.attempted, 1u);
    EXPECT_EQ(result.failed, 1u);
    EXPECT_EQ(result.decision, LuaHookDecision::kPass);
    EXPECT_NE(result.error.find("instruction limit exceeded"), std::string::npos);
    ASSERT_EQ(result.script_results.size(), 1u);
    EXPECT_TRUE(result.script_results.front().failed);
    EXPECT_EQ(result.script_results.front().failure_kind, LuaRuntime::ScriptFailureKind::kInstructionLimit);
    EXPECT_EQ(after.instruction_limit_hits, before.instruction_limit_hits + 1u);
    EXPECT_EQ(after.memory_limit_hits, before.memory_limit_hits);
}

TEST_F(LuaRuntimeTest, CallAllMemoryLimitIncrementsLimitHits) {
    LuaRuntime::Config cfg{};
    cfg.memory_limit_bytes = 256 * 1024;
    LuaRuntime runtime(cfg);

    const auto script_path = temp_dir_ / "limit_memory.lua";
    write_text(script_path,
               "function on_login(ctx)\n"
               "  local values = {}\n"
               "  for i = 1, 4096 do\n"
               "    values[i] = tostring(i) .. string.rep('x', 1024)\n"
               "  end\n"
               "  return { decision = 'pass' }\n"
               "end\n");

    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "limit_memory"});
    const auto reload = runtime.reload_scripts(scripts);

    const auto before = runtime.metrics_snapshot();
    const auto result = runtime.call_all("on_login");
    const auto after = runtime.metrics_snapshot();

    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.attempted, 1u);
    EXPECT_EQ(result.failed, 1u);
    EXPECT_EQ(result.decision, LuaHookDecision::kPass);
    EXPECT_FALSE(result.error.empty());
    ASSERT_EQ(result.script_results.size(), 1u);
    EXPECT_TRUE(result.script_results.front().failed);
    EXPECT_EQ(result.script_results.front().failure_kind, LuaRuntime::ScriptFailureKind::kMemoryLimit);
    EXPECT_EQ(after.memory_limit_hits, before.memory_limit_hits + 1u);
    EXPECT_EQ(after.instruction_limit_hits, before.instruction_limit_hits);
}

TEST_F(LuaRuntimeTest, ResetClearsRuntimeState) {
    LuaRuntime runtime;

    const auto script_path = temp_dir_ / "sample.lua";
    write_text(script_path, "return 1\n");

    (void)runtime.register_host_api(
        "server",
        "log_info",
        [](const LuaRuntime::HostArgs&, const LuaRuntime::HostCallContext&) {
            return LuaRuntime::HostCallResult{};
        });
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
