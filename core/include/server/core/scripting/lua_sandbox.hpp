#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace server::core::scripting::sandbox {

/**
 * @brief Engine-agnostic sandbox policy for Lua runtime wiring.
 */
struct Policy {
    std::vector<std::string> allowed_libraries;
    std::vector<std::string> forbidden_symbols;
    std::uint64_t instruction_limit{100'000};
    std::size_t memory_limit_bytes{1 * 1024 * 1024};
};

/** @brief Returns the default sandbox policy used by LuaRuntime. */
Policy default_policy();

/** @brief Returns true when the library token is allowed by policy. */
bool is_library_allowed(std::string_view library, const Policy& policy);

/** @brief Returns true when the symbol token is forbidden by policy. */
bool is_symbol_forbidden(std::string_view symbol, const Policy& policy);

} // namespace server::core::scripting::sandbox
