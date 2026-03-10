#pragma once

#include <string_view>

// 빌드 메타데이터는 CMake 단계에서 주입됩니다(`core/CMakeLists.txt` 참고).
// 소스 아카이브처럼 git 정보가 없는 환경에서도 빌드가 깨지지 않도록
// 안전한 fallback("unknown") 값을 함께 둡니다.
#ifndef BUILD_GIT_HASH
#define BUILD_GIT_HASH "unknown"
#endif

#ifndef BUILD_GIT_DESCRIBE
#define BUILD_GIT_DESCRIBE "unknown"
#endif

#ifndef BUILD_TIME_UTC
#define BUILD_TIME_UTC "unknown"
#endif

namespace server::core::build_info {

/**
 * @brief 빌드에 사용된 git commit hash를 반환합니다.
 * @return commit hash 문자열(`unknown` 가능)
 */
inline constexpr std::string_view git_hash() noexcept { return BUILD_GIT_HASH; }

/**
 * @brief `git describe` 결과를 반환합니다.
 * @return describe 문자열(`unknown` 가능)
 */
inline constexpr std::string_view git_describe() noexcept { return BUILD_GIT_DESCRIBE; }

/**
 * @brief 빌드 시각(UTC)을 반환합니다.
 * @return UTC 시각 문자열(`unknown` 가능)
 */
inline constexpr std::string_view build_time_utc() noexcept { return BUILD_TIME_UTC; }

/**
 * @brief build-info 값이 유효한지 판별합니다.
 * @param v 검사할 값
 * @return 비어 있지 않고 `unknown`이 아니면 true
 */
inline constexpr bool known(std::string_view v) noexcept {
    return !v.empty() && v != std::string_view("unknown");
}

/**
 * @brief git hash가 유효하게 주입되었는지 판별합니다.
 * @return git hash가 known 상태면 true
 */
inline constexpr bool has_git_hash() noexcept { return known(git_hash()); }

} // namespace server::core::build_info
