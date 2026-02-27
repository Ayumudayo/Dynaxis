#pragma once

#include <cstdint>

namespace server::core::protocol {

/** @brief 서버가 광고하는 주 프로토콜 버전입니다. */
inline constexpr std::uint16_t kProtocolVersionMajor = 1;
/** @brief 서버가 광고하는 부 프로토콜 버전입니다. */
inline constexpr std::uint16_t kProtocolVersionMinor = 1;

/**
 * @brief 클라이언트 버전이 서버와 호환되는지 판정합니다.
 *
 * 규칙:
 * - major는 반드시 일치해야 합니다.
 * - minor는 클라이언트가 서버보다 같거나 낮아야 합니다.
 */
inline constexpr bool is_protocol_version_compatible(std::uint16_t client_major,
                                                     std::uint16_t client_minor) noexcept {
    return client_major == kProtocolVersionMajor && client_minor <= kProtocolVersionMinor;
}

} // namespace server::core::protocol
