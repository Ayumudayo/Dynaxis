#pragma once

#include <cstddef>

namespace server::core {

struct SessionOptions {
    std::size_t recv_max_payload = 32 * 1024;   // bytes
    std::size_t send_queue_max   = 256 * 1024;  // bytes
    unsigned    heartbeat_interval_ms = 10'000; // 0이면 비활성
    unsigned    read_timeout_ms       = 15'000; // 0이면 비활성
    unsigned    write_timeout_ms      = 15'000; // 0이면 비활성
};

} // namespace server::core

