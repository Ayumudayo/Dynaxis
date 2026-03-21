#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace server::core::scripting::sandbox {

/**
 * @brief Lua runtime 결선에 사용하는 엔진 중립적 sandbox 정책입니다.
 *
 * 이 정책은 "어떤 라이브러리/심볼을 허용할 것인가"와 "실행 예산을 어디까지 줄 것인가"를
 * 한곳에 모아, 스크립트 기능 확장과 운영 안전성을 같은 계약 아래에서 다루게 합니다.
 */
struct Policy {
    std::vector<std::string> allowed_libraries;
    std::vector<std::string> forbidden_symbols;
    std::uint64_t instruction_limit{100'000};
    std::size_t memory_limit_bytes{1 * 1024 * 1024};
};

/** @brief `LuaRuntime`이 기본으로 사용하는 sandbox 정책을 반환합니다. */
Policy default_policy();

/** @brief 지정한 라이브러리 토큰이 정책상 허용되면 true를 반환합니다. */
bool is_library_allowed(std::string_view library, const Policy& policy);

/** @brief 지정한 심볼 토큰이 정책상 금지되면 true를 반환합니다. */
bool is_symbol_forbidden(std::string_view symbol, const Policy& policy);

} // namespace server::core::scripting::sandbox
