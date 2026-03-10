#include <gtest/gtest.h>

#include "server/core/scripting/script_watcher.hpp"
#include "server/core/scripting/lua_runtime.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace server::core::scripting {
namespace {

class ScriptWatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir_ = std::filesystem::temp_directory_path() / ("script_watcher_test_" + nonce);
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

    static void rewrite_text_with_tick(const std::filesystem::path& path, const std::string& text) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        write_text(path, text);
    }

    static std::vector<ScriptWatcher::ChangeEvent> poll_changes(ScriptWatcher& watcher) {
        std::vector<ScriptWatcher::ChangeEvent> changes;
        EXPECT_TRUE(watcher.poll([&changes](const ScriptWatcher::ChangeEvent& event) {
            changes.push_back(event);
        }));
        return changes;
    }

    static bool has_event(const std::vector<ScriptWatcher::ChangeEvent>& changes,
                          const std::string& file_name,
                          ScriptWatcher::ChangeKind kind) {
        for (const auto& event : changes) {
            if (event.kind == kind && event.path.filename().string() == file_name) {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path temp_dir_;
};

TEST_F(ScriptWatcherTest, TracksLuaLifecycleWithExtensionFilter) {
    ScriptWatcher::Config cfg{};
    cfg.scripts_dir = temp_dir_;
    cfg.extensions = {".lua"};

    ScriptWatcher watcher(cfg);

    auto changes = poll_changes(watcher);
    EXPECT_TRUE(changes.empty());

    write_text(temp_dir_ / "alpha.lua", "return 1");
    write_text(temp_dir_ / "ignore.txt", "ignore");

    changes = poll_changes(watcher);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(has_event(changes, "alpha.lua", ScriptWatcher::ChangeKind::kAdded));

    rewrite_text_with_tick(temp_dir_ / "alpha.lua", "return 2");
    changes = poll_changes(watcher);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(has_event(changes, "alpha.lua", ScriptWatcher::ChangeKind::kModified));

    std::error_code remove_ec;
    ASSERT_TRUE(std::filesystem::remove(temp_dir_ / "alpha.lua", remove_ec));
    ASSERT_FALSE(remove_ec) << remove_ec.message();

    changes = poll_changes(watcher);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(has_event(changes, "alpha.lua", ScriptWatcher::ChangeKind::kRemoved));
}

TEST_F(ScriptWatcherTest, LockPathSkipsPollingUntilLockReleased) {
    const auto lock_path = temp_dir_ / "watcher.lock";
    write_text(lock_path, "lock");
    write_text(temp_dir_ / "script.lua", "return 'hello'");

    ScriptWatcher::Config cfg{};
    cfg.scripts_dir = temp_dir_;
    cfg.extensions = {".lua"};
    cfg.lock_path = lock_path;

    ScriptWatcher watcher(cfg);

    auto changes = poll_changes(watcher);
    EXPECT_TRUE(changes.empty());

    std::error_code remove_ec;
    ASSERT_TRUE(std::filesystem::remove(lock_path, remove_ec));
    ASSERT_FALSE(remove_ec) << remove_ec.message();

    changes = poll_changes(watcher);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(has_event(changes, "script.lua", ScriptWatcher::ChangeKind::kAdded));
}

TEST_F(ScriptWatcherTest, RecursiveFlagControlsNestedFileDiscovery) {
    const auto nested_dir = temp_dir_ / "nested";
    ASSERT_TRUE(std::filesystem::create_directories(nested_dir));
    write_text(nested_dir / "deep.lua", "return 42");

    ScriptWatcher::Config non_recursive_cfg{};
    non_recursive_cfg.scripts_dir = temp_dir_;
    non_recursive_cfg.extensions = {".lua"};
    non_recursive_cfg.recursive = false;
    ScriptWatcher non_recursive(non_recursive_cfg);

    auto non_recursive_changes = poll_changes(non_recursive);
    EXPECT_TRUE(non_recursive_changes.empty());

    ScriptWatcher::Config recursive_cfg{};
    recursive_cfg.scripts_dir = temp_dir_;
    recursive_cfg.extensions = {".lua"};
    recursive_cfg.recursive = true;
    ScriptWatcher recursive(recursive_cfg);

    auto recursive_changes = poll_changes(recursive);
    ASSERT_EQ(recursive_changes.size(), 1u);
    EXPECT_TRUE(has_event(recursive_changes, "deep.lua", ScriptWatcher::ChangeKind::kAdded));
}

TEST_F(ScriptWatcherTest, FileModificationCanTriggerLuaRuntimeReloadFlow) {
    const auto script_path = temp_dir_ / "policy.lua";
    write_text(script_path,
               "return { hook = \"on_login\", decision = \"pass\" }\n");

    ScriptWatcher::Config cfg{};
    cfg.scripts_dir = temp_dir_;
    cfg.extensions = {".lua"};
    ScriptWatcher watcher(cfg);

    LuaRuntime runtime;
    std::vector<LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto initial_reload = runtime.reload_scripts(scripts);
    ASSERT_TRUE(initial_reload.error.empty());
    ASSERT_EQ(initial_reload.loaded, 1u);

    const auto baseline = runtime.call_all("on_login");
    EXPECT_TRUE(baseline.error.empty());
    EXPECT_EQ(baseline.decision, LuaHookDecision::kPass);

    (void)poll_changes(watcher);

    rewrite_text_with_tick(script_path,
                           "return { hook = \"on_login\", decision = \"deny\", reason = \"reloaded deny\" }\n");

    std::vector<ScriptWatcher::ChangeEvent> changes;
    bool reload_called = false;
    bool reload_ok = false;
    EXPECT_TRUE(watcher.poll([&](const ScriptWatcher::ChangeEvent& event) {
        changes.push_back(event);
        if (event.path.filename().string() != "policy.lua") {
            return;
        }

        reload_called = true;
        const auto reload_result = runtime.reload_scripts(scripts);
        reload_ok = reload_result.error.empty() && reload_result.failed == 0u
            && reload_result.loaded == 1u;
    }));

    EXPECT_TRUE(has_event(changes, "policy.lua", ScriptWatcher::ChangeKind::kModified));
    EXPECT_TRUE(reload_called);
    EXPECT_TRUE(reload_ok);

    const auto after_reload = runtime.call_all("on_login");
    EXPECT_TRUE(after_reload.error.empty());
    EXPECT_EQ(after_reload.decision, LuaHookDecision::kDeny);
    EXPECT_EQ(after_reload.reason, "reloaded deny");
}

} // namespace
} // namespace server::core::scripting
