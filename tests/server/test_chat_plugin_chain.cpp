#include <gtest/gtest.h>

#include "../../server/src/chat/chat_hook_plugin_chain.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#ifndef TEST_CHAT_HOOK_SAMPLE_PATH
#define TEST_CHAT_HOOK_SAMPLE_PATH ""
#endif

#ifndef TEST_CHAT_HOOK_SAMPLE_V2_PATH
#define TEST_CHAT_HOOK_SAMPLE_V2_PATH ""
#endif

#ifndef TEST_CHAT_HOOK_TAG_PATH
#define TEST_CHAT_HOOK_TAG_PATH ""
#endif

namespace server::app::chat {
namespace {

class ChatPluginChainTest : public ::testing::Test {
protected:
    void SetUp() override {
        sample_v1_module_ = std::filesystem::path(TEST_CHAT_HOOK_SAMPLE_PATH);
        sample_v2_module_ = std::filesystem::path(TEST_CHAT_HOOK_SAMPLE_V2_PATH);
        tag_module_ = std::filesystem::path(TEST_CHAT_HOOK_TAG_PATH);

        ASSERT_FALSE(sample_v1_module_.empty());
        ASSERT_FALSE(sample_v2_module_.empty());
        ASSERT_FALSE(tag_module_.empty());

        ASSERT_TRUE(std::filesystem::exists(sample_v1_module_));
        ASSERT_TRUE(std::filesystem::exists(sample_v2_module_));
        ASSERT_TRUE(std::filesystem::exists(tag_module_));

        const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir_ = std::filesystem::temp_directory_path() / ("chat_plugin_chain_test_" + nonce);
        plugins_dir_ = temp_dir_ / "plugins";
        cache_dir_ = temp_dir_ / "cache";
        ASSERT_TRUE(std::filesystem::create_directories(plugins_dir_));
        ASSERT_TRUE(std::filesystem::create_directories(cache_dir_));

        sample_live_ = plugins_dir_ / ("10_chat_hook_sample" + sample_v1_module_.extension().string());
        tag_live_ = plugins_dir_ / ("20_chat_hook_tag" + tag_module_.extension().string());

        std::error_code ec;
        std::filesystem::copy_file(sample_v1_module_, sample_live_, std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        ec.clear();
        std::filesystem::copy_file(tag_module_, tag_live_, std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        ChatHookPluginChain::Config cfg{};
        cfg.plugins_dir = plugins_dir_;
        cfg.cache_dir = cache_dir_;

        chain_ = std::make_unique<ChatHookPluginChain>(std::move(cfg));
        chain_->poll_reload();
    }

    void TearDown() override {
        chain_.reset();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    void swap_sample_to_v2() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        std::error_code ec;
        std::filesystem::copy_file(sample_v2_module_, sample_live_, std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        ec.clear();
        const auto current_mtime = std::filesystem::last_write_time(sample_live_, ec);
        ASSERT_FALSE(ec) << ec.message();

        ec.clear();
        std::filesystem::last_write_time(sample_live_, current_mtime + std::chrono::seconds(2), ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    static bool notice_contains(const ChatHookPluginChain::Outcome& out, const std::string& needle) {
        for (const auto& notice : out.notices) {
            if (notice.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path temp_dir_;
    std::filesystem::path plugins_dir_;
    std::filesystem::path cache_dir_;
    std::filesystem::path sample_live_;
    std::filesystem::path tag_live_;

    std::filesystem::path sample_v1_module_;
    std::filesystem::path sample_v2_module_;
    std::filesystem::path tag_module_;

    std::unique_ptr<ChatHookPluginChain> chain_;
};

TEST_F(ChatPluginChainTest, ReplaceTextDecisionUpdatesMessage) {
    std::string text = "/shout hello world";
    const auto out = chain_->on_chat_send(101, "lobby", "alice", text);

    EXPECT_FALSE(out.stop_default);
    EXPECT_EQ(text, "HELLO WORLD");
    EXPECT_TRUE(out.notices.empty());
}

TEST_F(ChatPluginChainTest, HandledDecisionStopsDefaultPath) {
    std::string text = "/plugin";
    const auto out = chain_->on_chat_send(102, "lobby", "alice", text);

    EXPECT_TRUE(out.stop_default);
    EXPECT_TRUE(notice_contains(out, "active version=v1"));
}

TEST_F(ChatPluginChainTest, BlockDecisionStopsDefaultPath) {
    std::string text = "banana";
    const auto out = chain_->on_chat_send(103, "lobby", "alice", text);

    EXPECT_TRUE(out.stop_default);
    EXPECT_TRUE(notice_contains(out, "blocked"));
    EXPECT_EQ(text, "banana");
}

TEST_F(ChatPluginChainTest, HotReloadSwapToV2ChangesBehaviorAndMetrics) {
    std::string before_text = "banana";
    const auto before = chain_->on_chat_send(104, "lobby", "alice", before_text);
    ASSERT_TRUE(before.stop_default);
    ASSERT_TRUE(notice_contains(before, "v1"));

    swap_sample_to_v2();
    chain_->poll_reload();

    std::string after_text = "banana";
    const auto after = chain_->on_chat_send(105, "lobby", "alice", after_text);
    EXPECT_FALSE(after.stop_default);
    EXPECT_EQ(after_text, "apple");
    EXPECT_TRUE(notice_contains(after, "v2"));

    const auto metrics = chain_->metrics_snapshot();
    bool found_sample = false;
    for (const auto& plugin : metrics.plugins) {
        if (plugin.plugin_path.filename().string().find("10_chat_hook_sample") == std::string::npos) {
            continue;
        }
        found_sample = true;
        EXPECT_TRUE(plugin.loaded);
        EXPECT_EQ(plugin.name, "chat_hook_sample");
        EXPECT_EQ(plugin.version, "v2");
        EXPECT_GE(plugin.reload_success_total, 2u);
        break;
    }

    EXPECT_TRUE(found_sample);
}

} // namespace
} // namespace server::app::chat
