#include <type_traits>

#include <iostream>

#include "server/core/fps/direct_bind.hpp"
#include "server/core/fps/direct_delivery.hpp"
#include "server/core/fps/runtime.hpp"
#include "server/core/fps/transport_policy.hpp"
#include "server/core/fps/transport_quality.hpp"

namespace {

bool require_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    static_assert(std::is_same_v<
        server::core::fps::WorldRuntime,
        server::core::realtime::WorldRuntime>);
    static_assert(std::is_same_v<
        server::core::fps::DirectBindRequest,
        server::core::realtime::DirectBindRequest>);
    static_assert(std::is_same_v<
        server::core::fps::DirectTransportRolloutPolicy,
        server::core::realtime::DirectTransportRolloutPolicy>);

    server::core::fps::DirectTransportRolloutPolicy policy;
    policy.enabled = true;
    policy.canary_percent = 100;
    policy.opcode_allowlist = server::core::fps::parse_direct_opcode_allowlist("0x0206");

    const auto attach = server::core::fps::evaluate_direct_attach(policy, "compat-session", 11);
    if (!require_true(
            attach.mode == server::core::realtime::DirectAttachMode::kRudpCanary,
            "fps wrapper should alias canonical realtime attach decision")) {
        return 1;
    }

    const auto delivery = server::core::fps::evaluate_direct_delivery(server::core::fps::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
    });
    if (!require_true(
            delivery.route == server::core::realtime::DirectDeliveryRoute::kUdp,
            "fps wrapper should alias canonical realtime delivery route")) {
        return 1;
    }

    server::core::fps::UdpSequencedMetrics metrics;
    const auto first = metrics.on_packet(1, 1000);
    if (!require_true(first.accepted, "fps wrapper should alias canonical realtime quality tracker")) {
        return 1;
    }

    const auto payload = server::core::fps::encode_direct_bind_request_payload(server::core::fps::DirectBindRequest{
        .session_id = "compat-session",
        .nonce = 11,
        .expires_unix_ms = 42,
        .token = "compat-token",
    });
    server::core::fps::DirectBindRequest decoded{};
    if (!require_true(
            server::core::fps::decode_direct_bind_request_payload(payload, decoded),
            "fps wrapper should preserve bind payload codec")) {
        return 1;
    }

    return 0;
}
