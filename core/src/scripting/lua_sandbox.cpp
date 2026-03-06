#include "server/core/scripting/lua_sandbox.hpp"

#include <cctype>

namespace server::core::scripting::sandbox {

namespace {

std::string normalize_token(std::string_view token) {
    std::size_t begin = 0;
    while (begin < token.size() && std::isspace(static_cast<unsigned char>(token[begin])) != 0) {
        ++begin;
    }

    std::size_t end = token.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1])) != 0) {
        --end;
    }

    std::string out;
    out.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(token[i]))));
    }
    return out;
}

bool contains_token(std::string_view token, const std::vector<std::string>& values) {
    const std::string normalized = normalize_token(token);
    if (normalized.empty()) {
        return false;
    }

    for (const auto& value : values) {
        if (normalize_token(value) == normalized) {
            return true;
        }
    }

    return false;
}

} // namespace

Policy default_policy() {
    Policy policy{};
    policy.allowed_libraries = {
        "base",
        "string",
        "table",
        "math",
        "utf8",
    };
    policy.forbidden_symbols = {
        "os",
        "io",
        "debug",
        "package",
        "ffi",
        "dofile",
        "loadfile",
        "require",
    };
    return policy;
}

bool is_library_allowed(std::string_view library, const Policy& policy) {
    return contains_token(library, policy.allowed_libraries);
}

bool is_symbol_forbidden(std::string_view symbol, const Policy& policy) {
    return contains_token(symbol, policy.forbidden_symbols);
}

} // namespace server::core::scripting::sandbox
