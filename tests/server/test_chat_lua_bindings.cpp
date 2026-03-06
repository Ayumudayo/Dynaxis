#include <gtest/gtest.h>

#include "server/core/scripting/lua_runtime.hpp"
#include "server/scripting/chat_lua_bindings.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

class FakeChatLuaHost : public server::app::scripting::ChatLuaHost {
public:
    std::optional<std::string> lua_get_user_name(std::uint32_t session_id) override {
        last_session_id = session_id;
        return std::string("alice");
    }

    std::optional<std::string> lua_get_user_room(std::uint32_t) override {
        return std::string("lobby");
    }

    std::vector<std::string> lua_get_room_users(std::string_view room_name) override {
        last_room = std::string(room_name);
        return {"alice", "bob"};
    }

    std::vector<std::string> lua_get_room_list() override {
        return {"lobby", "vip_lounge"};
    }

    std::optional<std::string> lua_get_room_owner(std::string_view) override {
        return std::string("alice");
    }

    bool lua_is_user_muted(std::string_view) override {
        return false;
    }

    bool lua_is_user_banned(std::string_view nickname) override {
        last_user = std::string(nickname);
        return false;
    }

    std::size_t lua_get_online_count() override {
        return 7;
    }

    std::size_t lua_get_room_count() override {
        return 2;
    }

    bool lua_send_notice(std::uint32_t session_id, std::string_view text) override {
        last_session_id = session_id;
        last_notice = std::string(text);
        return true;
    }

    bool lua_broadcast_room(std::string_view room_name, std::string_view text) override {
        last_room = std::string(room_name);
        last_notice = std::string(text);
        return true;
    }

    bool lua_broadcast_all(std::string_view text) override {
        last_notice = std::string(text);
        return true;
    }

    bool lua_kick_user(std::uint32_t session_id, std::string_view reason) override {
        last_session_id = session_id;
        last_notice = std::string(reason);
        return true;
    }

    bool lua_mute_user(std::string_view nickname,
                       std::uint32_t duration_sec,
                       std::string_view reason) override {
        last_user = std::string(nickname);
        last_duration_sec = duration_sec;
        last_notice = std::string(reason);
        return true;
    }

    bool lua_ban_user(std::string_view nickname,
                      std::uint32_t duration_sec,
                      std::string_view reason) override {
        last_user = std::string(nickname);
        last_duration_sec = duration_sec;
        last_notice = std::string(reason);
        return true;
    }

    std::uint32_t last_session_id{0};
    std::string last_room;
    std::string last_user;
    std::string last_notice;
    std::uint32_t last_duration_sec{0};
};

TEST(ChatLuaBindingsTest, RegistersExpectedBindingCount) {
    server::core::scripting::LuaRuntime runtime;
    FakeChatLuaHost host;
    const auto result = server::app::scripting::register_chat_lua_bindings(runtime, host);

    EXPECT_EQ(result.attempted, server::app::scripting::chat_lua_binding_count());
#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_EQ(result.registered, result.attempted);
#else
    EXPECT_EQ(result.registered, 0u);
#endif

    const auto metrics = runtime.metrics_snapshot();
    EXPECT_EQ(metrics.registered_host_api, result.registered);
}

TEST(ChatLuaBindingsTest, FunctionStyleScriptCanUseRegisteredBindings) {
    server::core::scripting::LuaRuntime runtime;
    FakeChatLuaHost host;
    const auto bindings = server::app::scripting::register_chat_lua_bindings(runtime, host);
    EXPECT_EQ(bindings.attempted, server::app::scripting::chat_lua_binding_count());

    const auto temp_dir = std::filesystem::temp_directory_path() / "knights_chat_lua_bindings_test.lua";
    {
        std::ofstream out(temp_dir, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "function on_login(ctx)\n"
               "  local name = server.get_user_name(ctx.session_id)\n"
               "  local users = server.get_room_users('lobby')\n"
               "  server.send_notice(ctx.session_id, name .. ':' .. tostring(#users))\n"
               "  return { decision = 'deny', reason = server.hook_name() .. ':' .. server.script_name() }\n"
               "end\n";
    }

    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back({temp_dir, "bindings"});
    const auto reload = runtime.reload_scripts(scripts);

    server::core::scripting::LuaHookContext ctx{};
    ctx.session_id = 42;
    ctx.user = "alice";
    const auto result = runtime.call_all("on_login", ctx);

#if KNIGHTS_BUILD_LUA_SCRIPTING
    EXPECT_TRUE(reload.error.empty());
    EXPECT_EQ(result.decision, server::core::scripting::LuaHookDecision::kDeny);
    EXPECT_EQ(result.reason, "on_login:bindings");
    EXPECT_EQ(host.last_session_id, 42u);
    EXPECT_EQ(host.last_room, "lobby");
    EXPECT_EQ(host.last_notice, "alice:2");
#else
    EXPECT_FALSE(reload.error.empty());
    EXPECT_FALSE(result.error.empty());
#endif

    std::error_code ec;
    std::filesystem::remove(temp_dir, ec);
}

} // namespace
