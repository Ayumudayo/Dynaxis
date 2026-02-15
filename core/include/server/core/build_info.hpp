#pragma once

#include <string_view>

// Build metadata injected by CMake (see `core/CMakeLists.txt`).
// Fallbacks are kept for builds from source archives without `.git`.
#ifndef KNIGHTS_GIT_HASH
#define KNIGHTS_GIT_HASH "unknown"
#endif

#ifndef KNIGHTS_GIT_DESCRIBE
#define KNIGHTS_GIT_DESCRIBE "unknown"
#endif

#ifndef KNIGHTS_BUILD_TIME_UTC
#define KNIGHTS_BUILD_TIME_UTC "unknown"
#endif

namespace server::core::build_info {

inline constexpr std::string_view git_hash() noexcept { return KNIGHTS_GIT_HASH; }
inline constexpr std::string_view git_describe() noexcept { return KNIGHTS_GIT_DESCRIBE; }
inline constexpr std::string_view build_time_utc() noexcept { return KNIGHTS_BUILD_TIME_UTC; }

inline constexpr bool known(std::string_view v) noexcept {
    return !v.empty() && v != std::string_view("unknown");
}

inline constexpr bool has_git_hash() noexcept { return known(git_hash()); }

} // namespace server::core::build_info
