#pragma once

#include <cstddef>

namespace server::core::scripting {
class LuaRuntime;
}

namespace server::app::scripting {

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
 * @return 등록 시도/성공 개수를 담은 결과
 */
ChatLuaBindingsResult register_chat_lua_bindings(server::core::scripting::LuaRuntime& runtime);

} // namespace server::app::scripting
