#pragma once

#include <atomic>
#include <cstddef>
#include <random>

namespace server::core::net {

/**
 * @brief Packet session layer shared runtime state.
 *
 * Tracks active connection count, optional max-capacity guardrail, and
 * randomized session-id seed to reduce cross-process collision risk.
 */
struct ConnectionRuntimeState {
    std::atomic<std::size_t> connection_count{0};
    std::size_t max_connections = 10'000;
    std::atomic<std::uint32_t> next_session_id;

    ConnectionRuntimeState() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<std::uint32_t> dis(1000, 0xFFFFFF00u);
        next_session_id.store(dis(gen));
    }
};

} // namespace server::core::net
