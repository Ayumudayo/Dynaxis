#include <gtest/gtest.h>

#include "server/scripting/chat_lua_bindings.hpp"
#include "server/core/scripting/lua_runtime.hpp"

TEST(ChatLuaBindingsTest, RegistersExpectedBindingCount) {
    server::core::scripting::LuaRuntime runtime;
    const auto result = server::app::scripting::register_chat_lua_bindings(runtime);

    EXPECT_EQ(result.attempted, server::app::scripting::chat_lua_binding_count());
#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_EQ(result.registered, result.attempted);
#else
    EXPECT_EQ(result.registered, 0u);
#endif

    const auto metrics = runtime.metrics_snapshot();
    EXPECT_EQ(metrics.registered_host_api, result.registered);
}
