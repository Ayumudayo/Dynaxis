#include <gtest/gtest.h>

#include "plugin_test_helpers.hpp"

#include <filesystem>
#include <string>
#include <vector>

#ifndef TEST_PLUGIN_RUNTIME_V1_FILE
#error "TEST_PLUGIN_RUNTIME_V1_FILE is not defined"
#endif

#ifndef TEST_PLUGIN_RUNTIME_V2_FILE
#error "TEST_PLUGIN_RUNTIME_V2_FILE is not defined"
#endif

namespace knights::tests::plugin {
namespace {

using Chain = server::core::plugin::PluginChainHost<TestPluginApi>;

class PluginChainHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        module_v1_path_ = resolve_module_path(TEST_PLUGIN_RUNTIME_V1_FILE);
        module_v2_path_ = resolve_module_path(TEST_PLUGIN_RUNTIME_V2_FILE);

        ASSERT_TRUE(std::filesystem::exists(module_v1_path_));
        ASSERT_TRUE(std::filesystem::exists(module_v2_path_));

        module_extension_ = module_v1_path_.extension().string();
        ASSERT_FALSE(module_extension_.empty());

        temp_dir_ = make_temp_dir("plugin_chain_test");
        plugins_dir_ = temp_dir_ / "plugins";
        cache_dir_ = temp_dir_ / "cache";

        ASSERT_TRUE(std::filesystem::create_directories(plugins_dir_));
        ASSERT_TRUE(std::filesystem::create_directories(cache_dir_));
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path stage(const std::filesystem::path& src, const std::string& name) const {
        const auto dst = plugins_dir_ / (name + module_extension_);
        std::error_code ec;
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        EXPECT_FALSE(ec) << ec.message();
        return dst;
    }

    static std::vector<std::string> loaded_plugin_names(const std::shared_ptr<const Chain::HostList>& chain) {
        std::vector<std::string> names;
        if (!chain) {
            return names;
        }

        names.reserve(chain->size());
        for (const auto& host : *chain) {
            if (!host) {
                continue;
            }
            const auto loaded = host->current();
            if (!loaded || !loaded->api || !loaded->api->name) {
                continue;
            }
            names.emplace_back(loaded->api->name);
        }

        return names;
    }

    std::filesystem::path temp_dir_;
    std::filesystem::path plugins_dir_;
    std::filesystem::path cache_dir_;
    std::string module_extension_;

    std::filesystem::path module_v1_path_;
    std::filesystem::path module_v2_path_;
};

TEST_F(PluginChainHostTest, DirectoryModeLoadsPluginsInFilenameOrder) {
    (void)stage(module_v2_path_, "20_second");
    (void)stage(module_v1_path_, "10_first");

    Chain chain(make_chain_config(cache_dir_, plugins_dir_));
    chain.poll_reload();

    const auto current = chain.current_chain();
    ASSERT_TRUE(current);
    ASSERT_EQ(current->size(), 2u);

    const auto names = loaded_plugin_names(current);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "plugin_v1");
    EXPECT_EQ(names[1], "plugin_v2");
}

TEST_F(PluginChainHostTest, DirectoryModeTracksPluginAddAndRemove) {
    const auto first = stage(module_v1_path_, "10_first");
    Chain chain(make_chain_config(cache_dir_, plugins_dir_));

    chain.poll_reload();
    auto current = chain.current_chain();
    ASSERT_TRUE(current);
    ASSERT_EQ(current->size(), 1u);

    (void)stage(module_v2_path_, "20_second");
    chain.poll_reload();

    current = chain.current_chain();
    ASSERT_TRUE(current);
    ASSERT_EQ(current->size(), 2u);

    std::error_code remove_ec;
    ASSERT_TRUE(std::filesystem::remove(first, remove_ec));
    ASSERT_FALSE(remove_ec) << remove_ec.message();

    chain.poll_reload();
    current = chain.current_chain();
    ASSERT_TRUE(current);
    ASSERT_EQ(current->size(), 1u);

    const auto names = loaded_plugin_names(current);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "plugin_v2");
}

TEST_F(PluginChainHostTest, ScanFailureKeepsPreviousChain) {
    (void)stage(module_v1_path_, "10_first");

    Chain chain(make_chain_config(cache_dir_, plugins_dir_));
    chain.poll_reload();

    const auto before = chain.current_chain();
    ASSERT_TRUE(before);
    ASSERT_EQ(before->size(), 1u);

    std::error_code remove_ec;
    std::filesystem::remove_all(plugins_dir_, remove_ec);
    ASSERT_FALSE(remove_ec) << remove_ec.message();

    chain.poll_reload();
    const auto after = chain.current_chain();
    ASSERT_TRUE(after);
    ASSERT_EQ(after->size(), 1u);

    const auto names = loaded_plugin_names(after);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "plugin_v1");
}

} // namespace
} // namespace knights::tests::plugin
