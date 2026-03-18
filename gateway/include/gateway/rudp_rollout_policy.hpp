#pragma once

#include <cstdint>

#include "server/core/fps/transport_policy.hpp"

namespace gateway {

inline auto parse_rudp_opcode_allowlist(std::string_view csv) {
    return server::core::fps::parse_direct_opcode_allowlist(csv);
}

inline auto parse_udp_opcode_allowlist(std::string_view csv) {
    return server::core::fps::parse_direct_opcode_allowlist(csv);
}

using RudpRolloutPolicy = server::core::fps::DirectTransportRolloutPolicy;

} // namespace gateway
