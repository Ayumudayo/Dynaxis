#include <gtest/gtest.h>

#include "plugin_test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#ifndef TEST_PLUGIN_RUNTIME_V1_FILE
#error "TEST_PLUGIN_RUNTIME_V1_FILE is not defined"
#endif

#ifndef TEST_PLUGIN_RUNTIME_V2_FILE
#error "TEST_PLUGIN_RUNTIME_V2_FILE is not defined"
#endif

#ifndef TEST_PLUGIN_RUNTIME_INVALID_FILE
#error "TEST_PLUGIN_RUNTIME_INVALID_FILE is not defined"
#endif

namespace knights::tests::plugin {
namespace {

class PluginHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        module_v1_path_ = resolve_module_path(TEST_PLUGIN_RUNTIME_V1_FILE);
        module_v2_path_ = resolve_module_path(TEST_PLUGIN_RUNTIME_V2_FILE);
        module_invalid_path_ = resolve_module_path(TEST_PLUGIN_RUNTIME_INVALID_FILE);

        ASSERT_TRUE(std::filesystem::exists(module_v1_path_));
        ASSERT_TRUE(std::filesystem::exists(module_v2_path_));
        ASSERT_TRUE(std::filesystem::exists(module_invalid_path_));

        temp_dir_ = make_temp_dir("plugin_host_test");
        cache_dir_ = temp_dir_ / "cache";
        ASSERT_TRUE(std::filesystem::create_directories(cache_dir_));

        live_plugin_path_ = temp_dir_ / ("active" + module_v1_path_.extension().string());
        std::error_code ec;
        std::filesystem::copy_file(
            module_v1_path_,
            live_plugin_path_,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        ASSERT_FALSE(ec) << ec.message();

        lock_path_ = temp_dir_ / "active_LOCK";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path temp_dir_;
    std::filesystem::path cache_dir_;
    std::filesystem::path live_plugin_path_;
    std::filesystem::path lock_path_;

    std::filesystem::path module_v1_path_;
    std::filesystem::path module_v2_path_;
    std::filesystem::path module_invalid_path_;
};

TEST_F(PluginHostTest, LoadsPluginAndSkipsReloadWhenMtimeUnchanged) {
    Host host(make_host_config(live_plugin_path_, cache_dir_));

    host.poll_reload();
    auto current = host.current();
    ASSERT_TRUE(current);
    ASSERT_NE(current->api, nullptr);
    EXPECT_EQ(current->api->transform(10), 11);
    EXPECT_STREQ(current->api->name, "plugin_v1");

    auto snap = host.metrics_snapshot();
    EXPECT_TRUE(snap.loaded);
    EXPECT_EQ(snap.reload_attempt_total, 1u);
    EXPECT_EQ(snap.reload_success_total, 1u);
    EXPECT_EQ(snap.reload_failure_total, 0u);

    host.poll_reload();
    snap = host.metrics_snapshot();
    EXPECT_EQ(snap.reload_attempt_total, 1u);
    EXPECT_EQ(snap.reload_success_total, 1u);
    EXPECT_EQ(snap.reload_failure_total, 0u);
}

TEST_F(PluginHostTest, LockFileDefersReloadUntilReleased) {
    {
        std::ofstream lock_file(lock_path_);
        ASSERT_TRUE(lock_file.good());
    }

    Host host(make_host_config(live_plugin_path_, cache_dir_, lock_path_));

    host.poll_reload();
    auto snap = host.metrics_snapshot();
    EXPECT_FALSE(snap.loaded);
    EXPECT_EQ(snap.reload_attempt_total, 0u);
    EXPECT_EQ(snap.reload_success_total, 0u);
    EXPECT_EQ(snap.reload_failure_total, 0u);

    std::error_code ec;
    ASSERT_TRUE(std::filesystem::remove(lock_path_, ec));
    ASSERT_FALSE(ec) << ec.message();

    host.poll_reload();
    snap = host.metrics_snapshot();
    EXPECT_TRUE(snap.loaded);
    EXPECT_EQ(snap.reload_attempt_total, 1u);
    EXPECT_EQ(snap.reload_success_total, 1u);
    EXPECT_EQ(snap.reload_failure_total, 0u);
}

TEST_F(PluginHostTest, ValidatorFailureKeepsPreviousLoadedPlugin) {
    Host host(make_host_config(live_plugin_path_, cache_dir_));

    host.poll_reload();
    const auto before = host.current();
    ASSERT_TRUE(before);
    ASSERT_NE(before->api, nullptr);
    EXPECT_EQ(before->api->transform(10), 11);

    std::string copy_error;
    ASSERT_TRUE(copy_with_mtime_tick(module_invalid_path_, live_plugin_path_, copy_error)) << copy_error;

    host.poll_reload();
    const auto after = host.current();
    ASSERT_TRUE(after);
    ASSERT_NE(after->api, nullptr);
    EXPECT_EQ(after.get(), before.get());
    EXPECT_EQ(after->api->transform(10), 11);
    EXPECT_STREQ(after->api->name, "plugin_v1");

    const auto snap = host.metrics_snapshot();
    EXPECT_EQ(snap.reload_attempt_total, 2u);
    EXPECT_EQ(snap.reload_success_total, 1u);
    EXPECT_EQ(snap.reload_failure_total, 1u);
}

TEST_F(PluginHostTest, SuccessfulSwapReplacesCurrentPlugin) {
    Host host(make_host_config(live_plugin_path_, cache_dir_));

    host.poll_reload();
    const auto before = host.current();
    ASSERT_TRUE(before);
    ASSERT_NE(before->api, nullptr);
    EXPECT_EQ(before->api->transform(10), 11);

    std::string copy_error;
    ASSERT_TRUE(copy_with_mtime_tick(module_v2_path_, live_plugin_path_, copy_error)) << copy_error;

    host.poll_reload();
    const auto after = host.current();
    ASSERT_TRUE(after);
    ASSERT_NE(after->api, nullptr);
    EXPECT_NE(after.get(), before.get());
    EXPECT_EQ(after->api->transform(10), 12);
    EXPECT_STREQ(after->api->name, "plugin_v2");

    const auto snap = host.metrics_snapshot();
    EXPECT_EQ(snap.reload_attempt_total, 2u);
    EXPECT_EQ(snap.reload_success_total, 2u);
    EXPECT_EQ(snap.reload_failure_total, 0u);
}

} // namespace
} // namespace knights::tests::plugin
