#pragma once

#include <cstddef>

namespace server::core::net {

inline constexpr bool exceeds_queue_budget(std::size_t queue_max_bytes,
                                           std::size_t queued_bytes,
                                           std::size_t incoming_bytes) noexcept {
    if (queue_max_bytes == 0) {
        return false;
    }
    if (incoming_bytes > queue_max_bytes) {
        return true;
    }
    return queued_bytes > (queue_max_bytes - incoming_bytes);
}

} // namespace server::core::net
