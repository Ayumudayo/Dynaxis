#include "server/core/scripting/lua_sandbox.hpp"

#include <gtest/gtest.h>

namespace server::core::scripting::sandbox {
namespace {

TEST(LuaSandboxTest, DefaultPolicyAllowsExpectedLibraries) {
    const auto policy = default_policy();

    EXPECT_TRUE(is_library_allowed("base", policy));
    EXPECT_TRUE(is_library_allowed("string", policy));
    EXPECT_TRUE(is_library_allowed("table", policy));
    EXPECT_TRUE(is_library_allowed("math", policy));
    EXPECT_TRUE(is_library_allowed("utf8", policy));

    EXPECT_FALSE(is_library_allowed("os", policy));
    EXPECT_FALSE(is_library_allowed("io", policy));
    EXPECT_FALSE(is_library_allowed("debug", policy));
    EXPECT_FALSE(is_library_allowed("package", policy));
    EXPECT_FALSE(is_library_allowed("ffi", policy));
}

TEST(LuaSandboxTest, DefaultPolicyForbidsDangerousSymbols) {
    const auto policy = default_policy();

    EXPECT_TRUE(is_symbol_forbidden("os", policy));
    EXPECT_TRUE(is_symbol_forbidden("io", policy));
    EXPECT_TRUE(is_symbol_forbidden("debug", policy));
    EXPECT_TRUE(is_symbol_forbidden("package", policy));
    EXPECT_TRUE(is_symbol_forbidden("ffi", policy));
    EXPECT_TRUE(is_symbol_forbidden("dofile", policy));
    EXPECT_TRUE(is_symbol_forbidden("loadfile", policy));
    EXPECT_TRUE(is_symbol_forbidden("require", policy));

    EXPECT_FALSE(is_symbol_forbidden("pairs", policy));
    EXPECT_FALSE(is_symbol_forbidden("ipairs", policy));
}

TEST(LuaSandboxTest, DefaultPolicyHasExpectedExecutionLimits) {
    const auto policy = default_policy();

    EXPECT_EQ(policy.instruction_limit, 100'000u);
    EXPECT_EQ(policy.memory_limit_bytes, 1u * 1024u * 1024u);
}

TEST(LuaSandboxTest, TokenChecksAreWhitespaceAndCaseInsensitive) {
    const auto policy = default_policy();

    EXPECT_TRUE(is_library_allowed("  MaTh  ", policy));
    EXPECT_TRUE(is_symbol_forbidden("  LoAdFiLe  ", policy));
    EXPECT_FALSE(is_library_allowed("   ", policy));
    EXPECT_FALSE(is_symbol_forbidden("   ", policy));
}

} // namespace
} // namespace server::core::scripting::sandbox
