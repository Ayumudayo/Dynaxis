#pragma once

#include <atomic>
#include <cstddef>

namespace server::core {

struct SharedState {
    std::atomic<std::size_t> connection_count{0};
    std::size_t max_connections = 10'000; // 0이면 무제한
};

} // namespace server::core

