#include <gtest/gtest.h>

#include "../../server/src/chat/chat_hook_plugin_chain.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifndef TEST_CHAT_HOOK_SAMPLE_V2_PATH
#define TEST_CHAT_HOOK_SAMPLE_V2_PATH ""
#endif

#ifndef TEST_CHAT_HOOK_TAG_PATH
#define TEST_CHAT_HOOK_TAG_PATH ""
#endif

#ifndef TEST_CHAT_HOOK_V2_ONLY_PATH
#define TEST_CHAT_HOOK_V2_ONLY_PATH ""
#endif

#ifndef TEST_CHAT_HOOK_DUAL_PATH
#define TEST_CHAT_HOOK_DUAL_PATH ""
#endif

namespace server::app::chat {
namespace {

class ChatPluginChainV2Test : public ::testing::Test {
protected:
    void SetUp() override {
        sample_v2_module_ = std::filesystem::path(TEST_CHAT_HOOK_SAMPLE_V2_PATH);
        tag_module_ = std::filesystem::path(TEST_CHAT_HOOK_TAG_PATH);
        v2_only_module_ = std::filesystem::path(TEST_CHAT_HOOK_V2_ONLY_PATH);
        dual_module_ = std::filesystem::path(TEST_CHAT_HOOK_DUAL_PATH);

        ASSERT_FALSE(sample_v2_module_.empty());
        ASSERT_FALSE(tag_module_.empty());
        ASSERT_FALSE(v2_only_module_.empty());
        ASSERT_FALSE(dual_module_.empty());

        ASSERT_TRUE(std::filesystem::exists(sample_v2_module_));
        ASSERT_TRUE(std::filesystem::exists(tag_module_));
        ASSERT_TRUE(std::filesystem::exists(v2_only_module_));
        ASSERT_TRUE(std::filesystem::exists(dual_module_));

        const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir_ = std::filesystem::temp_directory_path() / ("knights_chat_plugin_chain_v2_test_" + nonce);
        plugins_dir_ = temp_dir_ / "plugins";
        cache_dir_ = temp_dir_ / "cache";
        ASSERT_TRUE(std::filesystem::create_directories(plugins_dir_));
        ASSERT_TRUE(std::filesystem::create_directories(cache_dir_));

        build_chain({
            {v2_only_module_, "10_chat_hook_v2_only"},
            {tag_module_, "20_chat_hook_tag"},
        });
    }

    void TearDown() override {
        chain_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    void build_chain(const std::vector<std::pair<std::filesystem::path, std::string>>& modules) {
        chain_.reset();

        std::error_code ec;
        std::filesystem::remove_all(plugins_dir_, ec);
        ec.clear();
        std::filesystem::remove_all(cache_dir_, ec);

        ASSERT_TRUE(std::filesystem::create_directories(plugins_dir_));
        ASSERT_TRUE(std::filesystem::create_directories(cache_dir_));

        for (const auto& [src, file_stem] : modules) {
            const auto dst = plugins_dir_ / (file_stem + src.extension().string());
            ec.clear();
            std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << ec.message();
        }

        ChatHookPluginChain::Config cfg{};
        cfg.plugins_dir = plugins_dir_;
        cfg.cache_dir = cache_dir_;

        chain_ = std::make_unique<ChatHookPluginChain>(std::move(cfg));
        chain_->poll_reload();
    }

    static bool notice_contains(const ChatHookPluginChain::Outcome& out, const std::string& needle) {
        for (const auto& notice : out.notices) {
            if (notice.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    static bool notice_contains(const ChatHookPluginChain::GateOutcome& out, const std::string& needle) {
        for (const auto& notice : out.notices) {
            if (notice.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    static bool notice_contains(const ChatHookPluginChain::AdminOutcome& out, const std::string& needle) {
        for (const auto& notice : out.notices) {
            if (notice.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    static const ChatHookPluginChain::PluginMetricsSnapshot* find_plugin(
        const ChatHookPluginChain::MetricsSnapshot& metrics,
        const std::string& file_stem) {
        for (const auto& plugin : metrics.plugins) {
            if (plugin.plugin_path.filename().string().find(file_stem) != std::string::npos) {
                return &plugin;
            }
        }
        return nullptr;
    }

    std::filesystem::path temp_dir_;
    std::filesystem::path plugins_dir_;
    std::filesystem::path cache_dir_;

    std::filesystem::path sample_v2_module_;
    std::filesystem::path tag_module_;
    std::filesystem::path v2_only_module_;
    std::filesystem::path dual_module_;

    std::unique_ptr<ChatHookPluginChain> chain_;
};

TEST_F(ChatPluginChainV2Test, LoadsPluginWithV2OnlyEntrypoint) {
    std::string text = "/v2only";
    const auto out = chain_->on_chat_send(201, "lobby", "alice", text);

    EXPECT_TRUE(out.stop_default);
    EXPECT_TRUE(notice_contains(out, "v2 entrypoint"));

    const auto metrics = chain_->metrics_snapshot();
    const auto* plugin = find_plugin(metrics, "10_chat_hook_v2_only");
    ASSERT_NE(plugin, nullptr);
    EXPECT_TRUE(plugin->loaded);
    EXPECT_EQ(plugin->name, "chat_hook_v2_only");
    EXPECT_EQ(plugin->version, "v2");
    EXPECT_GE(plugin->reload_success_total, 1u);
}

TEST_F(ChatPluginChainV2Test, FallsBackToV1PluginWhenV2EntrypointMissing) {
    std::string text = "/tag hello";
    const auto out = chain_->on_chat_send(202, "lobby", "alice", text);

    EXPECT_FALSE(out.stop_default);
    EXPECT_EQ(text, "[tag] hello");

    const auto metrics = chain_->metrics_snapshot();
    const auto* tag = find_plugin(metrics, "20_chat_hook_tag");
    ASSERT_NE(tag, nullptr);
    EXPECT_TRUE(tag->loaded);
    EXPECT_EQ(tag->name, "chat_hook_tag");
    EXPECT_EQ(tag->version, "v1");
    EXPECT_GE(tag->reload_success_total, 1u);
}

TEST_F(ChatPluginChainV2Test, PrefersV2EntrypointWhenBothSymbolsExist) {
    build_chain({
        {dual_module_, "10_chat_hook_dual_entrypoint"},
    });

    std::string text = "/dual";
    const auto out = chain_->on_chat_send(205, "lobby", "alice", text);

    EXPECT_TRUE(out.stop_default);
    EXPECT_TRUE(notice_contains(out, "via v2 entrypoint"));
    EXPECT_FALSE(notice_contains(out, "via v1 entrypoint"));

    const auto metrics = chain_->metrics_snapshot();
    const auto* dual = find_plugin(metrics, "10_chat_hook_dual_entrypoint");
    ASSERT_NE(dual, nullptr);
    EXPECT_TRUE(dual->loaded);
    EXPECT_EQ(dual->name, "chat_hook_dual_entrypoint");
    EXPECT_EQ(dual->version, "v2-entrypoint");
}

TEST_F(ChatPluginChainV2Test, MixedV2AndV1ChainAppliesInOrder) {
    build_chain({
        {sample_v2_module_, "10_chat_hook_sample"},
        {tag_module_, "20_chat_hook_tag"},
    });

    std::string plugin_cmd = "/plugin";
    const auto plugin_out = chain_->on_chat_send(203, "lobby", "alice", plugin_cmd);
    EXPECT_TRUE(plugin_out.stop_default);
    EXPECT_TRUE(notice_contains(plugin_out, "active version=v2"));

    std::string tag_cmd = "/tag world";
    const auto tag_out = chain_->on_chat_send(204, "lobby", "alice", tag_cmd);
    EXPECT_FALSE(tag_out.stop_default);
    EXPECT_EQ(tag_cmd, "[tag] world");

    const auto metrics = chain_->metrics_snapshot();
    const auto* sample = find_plugin(metrics, "10_chat_hook_sample");
    ASSERT_NE(sample, nullptr);
    EXPECT_TRUE(sample->loaded);
    EXPECT_EQ(sample->name, "chat_hook_sample");
    EXPECT_EQ(sample->version, "v2");

    const auto* tag = find_plugin(metrics, "20_chat_hook_tag");
    ASSERT_NE(tag, nullptr);
    EXPECT_TRUE(tag->loaded);
    EXPECT_EQ(tag->name, "chat_hook_tag");
    EXPECT_EQ(tag->version, "v1");
}

TEST_F(ChatPluginChainV2Test, SampleV2PluginImplementsLoginGateExample) {
    build_chain({
        {sample_v2_module_, "10_chat_hook_sample"},
    });

    const auto denied = chain_->on_login(230, "sample_deny_login");
    EXPECT_TRUE(denied.stop_default);
    EXPECT_EQ(denied.deny_reason, "sample policy denied login");
    EXPECT_TRUE(notice_contains(denied, "login denied by sample policy"));

    const auto observed = chain_->on_login(231, "sample_login_notice");
    EXPECT_FALSE(observed.stop_default);
    EXPECT_TRUE(notice_contains(observed, "login hook observed user"));
}

TEST_F(ChatPluginChainV2Test, V2LoginHookCanDenyWithReason) {
    const auto out = chain_->on_login(301, "deny_login");

    EXPECT_TRUE(out.stop_default);
    EXPECT_EQ(out.deny_reason, "login blocked by v2-only test plugin");
    EXPECT_TRUE(notice_contains(out, "login blocked"));
}

TEST_F(ChatPluginChainV2Test, V2JoinHookCanDenyWithReason) {
    const auto out = chain_->on_join(302, "alice", "forbidden_room");

    EXPECT_TRUE(out.stop_default);
    EXPECT_EQ(out.deny_reason, "join blocked by v2-only test plugin");
    EXPECT_TRUE(notice_contains(out, "join blocked"));
}

TEST_F(ChatPluginChainV2Test, V2AdminHookCanDenyCommand) {
    const auto out = chain_->on_admin_command(
        "disconnect_users",
        "control-plane",
        "{\"users\":[\"alice\"],\"reason\":\"test\"}");

    EXPECT_TRUE(out.stop_default);
    EXPECT_EQ(out.deny_reason, "disconnect denied by v2-only test plugin");
    EXPECT_TRUE(notice_contains(out, "admin deny"));
}

TEST_F(ChatPluginChainV2Test, V2LeaveHookCanDeny) {
    const auto out = chain_->on_leave(303, "alice", "locked_leave");

    EXPECT_TRUE(out.stop_default);
}

TEST_F(ChatPluginChainV2Test, V2SessionEventHookPassesByDefault) {
    const auto out = chain_->on_session_event(304,
                                              SessionEventKindV2::kClose,
                                              "alice",
                                              "connection_closed");

    EXPECT_FALSE(out.stop_default);
    EXPECT_TRUE(out.notices.empty());
}

} // namespace
} // namespace server::app::chat
