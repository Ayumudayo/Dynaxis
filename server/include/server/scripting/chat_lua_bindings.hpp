#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace server::core::scripting {
class LuaRuntime;
}

namespace server::app::scripting {

/**
 * @brief Server-facing host interface used by Lua binding registration.
 *
 * Implementations must keep all action methods non-blocking from the caller's perspective.
 */
class ChatLuaHost {
public:
    virtual ~ChatLuaHost() = default;

    virtual std::optional<std::string> lua_get_user_name(std::uint32_t session_id) = 0;
    virtual std::optional<std::string> lua_get_user_room(std::uint32_t session_id) = 0;
    virtual std::vector<std::string> lua_get_room_users(std::string_view room_name) = 0;
    virtual std::vector<std::string> lua_get_room_list() = 0;
    virtual std::optional<std::string> lua_get_room_owner(std::string_view room_name) = 0;
    virtual bool lua_is_user_muted(std::string_view nickname) = 0;
    virtual bool lua_is_user_banned(std::string_view nickname) = 0;
    virtual std::size_t lua_get_online_count() = 0;
    virtual std::size_t lua_get_room_count() = 0;

    virtual bool lua_send_notice(std::uint32_t session_id, std::string_view text) = 0;
    virtual bool lua_broadcast_room(std::string_view room_name, std::string_view text) = 0;
    virtual bool lua_broadcast_all(std::string_view text) = 0;
    virtual bool lua_kick_user(std::uint32_t session_id, std::string_view reason) = 0;
    virtual bool lua_mute_user(std::string_view nickname,
                               std::uint32_t duration_sec,
                               std::string_view reason) = 0;
    virtual bool lua_ban_user(std::string_view nickname,
                              std::uint32_t duration_sec,
                              std::string_view reason) = 0;
};

/** @brief Chat Lua host API 바인딩 등록 결과입니다. */
struct ChatLuaBindingsResult {
    std::size_t attempted{0};
    std::size_t registered{0};
};

/**
 * @brief Chat Lua host API에서 등록을 시도하는 바인딩 개수를 반환합니다.
 * @return 등록 시도 대상 바인딩 개수
 */
std::size_t chat_lua_binding_count();

/**
 * @brief Chat Lua host API 바인딩을 런타임에 등록합니다.
 * @param runtime 바인딩을 등록할 LuaRuntime 인스턴스
 * @param host 바인딩 콜백이 위임할 서버 host 구현체
 * @return 등록 시도/성공 개수를 담은 결과
 */
ChatLuaBindingsResult register_chat_lua_bindings(server::core::scripting::LuaRuntime& runtime,
                                                 ChatLuaHost& host);

} // namespace server::app::scripting
