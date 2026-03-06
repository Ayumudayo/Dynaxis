#include <gtest/gtest.h>

#include "plugin_test_api.hpp"
#include "plugin_test_helpers.hpp"
#include "server/core/plugin/shared_library.hpp"

#include <filesystem>
#include <string>
#include <utility>

#ifndef TEST_PLUGIN_RUNTIME_V1_FILE
#error "TEST_PLUGIN_RUNTIME_V1_FILE is not defined"
#endif

namespace knights::tests::plugin {
namespace {

class SharedLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        module_v1_path_ = resolve_module_path(TEST_PLUGIN_RUNTIME_V1_FILE);
        ASSERT_TRUE(std::filesystem::exists(module_v1_path_));

        temp_dir_ = make_temp_dir("shared_library_test");
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path temp_dir_;
    std::filesystem::path module_v1_path_;
};

TEST_F(SharedLibraryTest, OpenResolveAndCloseWorks) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    EXPECT_FALSE(library.is_loaded());
    ASSERT_TRUE(library.open(module_v1_path_, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(library.is_loaded());

    void* symbol = library.symbol(kEntrypointSymbol, error);
    ASSERT_NE(symbol, nullptr) << error;
    EXPECT_TRUE(error.empty());

    auto get_api = reinterpret_cast<GetApiFn>(symbol);
    const TestPluginApi* api = get_api();
    ASSERT_NE(api, nullptr);
    EXPECT_EQ(api->abi_version, kExpectedAbiVersion);
    EXPECT_STREQ(api->name, "plugin_v1");

    library.close();
    EXPECT_FALSE(library.is_loaded());

    symbol = library.symbol(kEntrypointSymbol, error);
    EXPECT_EQ(symbol, nullptr);
    EXPECT_EQ(error, "library not loaded");
}

TEST_F(SharedLibraryTest, SymbolBeforeOpenReportsLibraryNotLoaded) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    EXPECT_FALSE(library.is_loaded());
    void* symbol = library.symbol(kEntrypointSymbol, error);
    EXPECT_EQ(symbol, nullptr);
    EXPECT_EQ(error, "library not loaded");
}

TEST_F(SharedLibraryTest, EmptySymbolNameIsRejected) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    ASSERT_TRUE(library.open(module_v1_path_, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(library.is_loaded());

    void* symbol = library.symbol("", error);
    EXPECT_EQ(symbol, nullptr);
    EXPECT_EQ(error, "symbol name is empty");

    symbol = library.symbol(nullptr, error);
    EXPECT_EQ(symbol, nullptr);
    EXPECT_EQ(error, "symbol name is empty");
}

TEST_F(SharedLibraryTest, MissingFileOpenFailsAndKeepsUnloadedState) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    const auto missing_path = temp_dir_ / "missing_test_plugin_module";
    ASSERT_FALSE(std::filesystem::exists(missing_path));

    EXPECT_FALSE(library.open(missing_path, error));
    EXPECT_FALSE(library.is_loaded());
    EXPECT_FALSE(error.empty());
}

TEST_F(SharedLibraryTest, MissingSymbolReturnsErrorAndKeepsLoadedLibrary) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    ASSERT_TRUE(library.open(module_v1_path_, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(library.is_loaded());

    void* symbol = library.symbol("knights_symbol_that_does_not_exist", error);
    EXPECT_EQ(symbol, nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(library.is_loaded());
}

TEST_F(SharedLibraryTest, FailedOpenAfterSuccessfulLoadResetsHandle) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    ASSERT_TRUE(library.open(module_v1_path_, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(library.is_loaded());

    const auto missing_path = temp_dir_ / "missing_after_loaded";
    EXPECT_FALSE(library.open(missing_path, error));
    EXPECT_FALSE(library.is_loaded());
    EXPECT_FALSE(error.empty());
}

TEST_F(SharedLibraryTest, OpenRecoversAfterPreviousFailure) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    const auto missing_path = temp_dir_ / "missing_then_recover";
    EXPECT_FALSE(library.open(missing_path, error));
    EXPECT_FALSE(library.is_loaded());
    EXPECT_FALSE(error.empty());

    ASSERT_TRUE(library.open(module_v1_path_, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(library.is_loaded());

    void* symbol = library.symbol(kEntrypointSymbol, error);
    ASSERT_NE(symbol, nullptr) << error;
    EXPECT_TRUE(error.empty());
}

TEST_F(SharedLibraryTest, CloseIsIdempotent) {
    server::core::plugin::SharedLibrary library;
    std::string error;

    ASSERT_TRUE(library.open(module_v1_path_, error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(library.is_loaded());

    library.close();
    library.close();

    EXPECT_FALSE(library.is_loaded());
    void* symbol = library.symbol(kEntrypointSymbol, error);
    EXPECT_EQ(symbol, nullptr);
    EXPECT_EQ(error, "library not loaded");
}

TEST_F(SharedLibraryTest, MoveOperationsTransferOwnership) {
    server::core::plugin::SharedLibrary source;
    std::string error;

    ASSERT_TRUE(source.open(module_v1_path_, error)) << error;
    EXPECT_TRUE(error.empty());
    ASSERT_TRUE(source.is_loaded());

    server::core::plugin::SharedLibrary moved(std::move(source));
    EXPECT_FALSE(source.is_loaded());
    EXPECT_TRUE(moved.is_loaded());

    void* symbol = moved.symbol(kEntrypointSymbol, error);
    ASSERT_NE(symbol, nullptr) << error;
    EXPECT_TRUE(error.empty());

    server::core::plugin::SharedLibrary assigned;
    assigned = std::move(moved);
    EXPECT_FALSE(moved.is_loaded());
    EXPECT_TRUE(assigned.is_loaded());

    symbol = assigned.symbol(kEntrypointSymbol, error);
    ASSERT_NE(symbol, nullptr) << error;
    EXPECT_TRUE(error.empty());
}

} // namespace
} // namespace knights::tests::plugin
