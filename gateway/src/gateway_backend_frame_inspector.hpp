#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace gateway {

std::vector<std::uint8_t> inspect_backend_frames(
    std::vector<std::uint8_t>& prebuffer,
    std::span<const std::uint8_t> payload,
    const std::function<void(std::span<const std::uint8_t>)>& on_login_res,
    const std::function<bool(std::uint16_t, std::span<const std::uint8_t>)>& on_frame);

} // namespace gateway
