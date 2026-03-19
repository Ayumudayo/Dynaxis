#pragma once

#include <cstdint>
#include <string_view>

namespace server::core::api {

/** @brief 공개 API 시맨틱 버전의 메이저 값입니다. */
inline constexpr std::uint32_t k_version_major = 2;
/** @brief 공개 API 시맨틱 버전의 마이너 값입니다. */
inline constexpr std::uint32_t k_version_minor = 23;
/** @brief 공개 API 시맨틱 버전의 패치 값입니다. */
inline constexpr std::uint32_t k_version_patch = 0;

/**
 * @brief 공개 API 표면의 시맨틱 버전 문자열을 반환합니다.
 * @return `major.minor.patch` 형식의 SemVer 문자열 리터럴
 */
inline constexpr std::string_view version_string() noexcept { return "2.23.0"; }

} // namespace server::core::api
