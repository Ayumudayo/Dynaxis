#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "server/core/api/version.hpp"
#include "server/core/realtime/direct_bind.hpp"
#include "server/core/realtime/direct_delivery.hpp"
#include "server/core/realtime/simulation_phase.hpp"
#include "server/core/realtime/transport_quality.hpp"
#include "server/core/realtime/runtime.hpp"
#include "server/core/realtime/transport_policy.hpp"

namespace {

bool require_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

class SmokePhaseObserver final : public server::core::realtime::ISimulationPhaseObserver {
public:
    void on_simulation_phase(server::core::realtime::SimulationPhase,
                             const server::core::realtime::SimulationPhaseContext&) override {
        ++event_count;
    }

    std::size_t event_count{0};
};

} // namespace

int main() {
    (void)server::core::api::version_string();
    server::core::realtime::SimulationPhaseContext phase_context{
        .server_tick = 1,
        .actor_count = 0,
        .viewer_count = 0,
        .staged_input_count = 0,
        .replication_update_count = 0,
    };
    (void)phase_context;

    server::core::realtime::FixedStepDriver driver(30, 4);
    if (!require_true(
            driver.consume_elapsed(std::chrono::milliseconds(200)) == 4,
            "fixed-step driver should bound catch-up work")) {
        return 1;
    }

    server::core::realtime::WorldRuntime runtime(server::core::realtime::RuntimeConfig{
        .max_delta_actors_per_tick = 1,
    });
    SmokePhaseObserver observer;
    runtime.set_simulation_phase_observer(&observer);

    const auto staged_1 = runtime.stage_input(1, server::core::realtime::InputCommand{
        .input_seq = 1,
        .move_x_mm = 100,
    });
    if (!require_true(
            staged_1.disposition == server::core::realtime::StageInputDisposition::kAccepted,
            "first staged input should be accepted")) {
        return 1;
    }
    if (!require_true(staged_1.target_server_tick == 1, "first staged input should target tick 1")) {
        return 1;
    }

    const auto staged_2 = runtime.stage_input(2, server::core::realtime::InputCommand{
        .input_seq = 1,
        .move_x_mm = 150,
    });
    if (!require_true(
            staged_2.disposition == server::core::realtime::StageInputDisposition::kAccepted,
            "second staged input should be accepted")) {
        return 1;
    }

    const auto first_tick = runtime.tick();
    if (!require_true(first_tick.size() == 2, "first tick should emit one snapshot per viewer")) {
        return 1;
    }
    for (const auto& update : first_tick) {
        if (!require_true(
                update.kind == server::core::realtime::ReplicationKind::kSnapshot,
                "first tick should emit snapshots")) {
            return 1;
        }
    }

    const auto actor_id = runtime.actor_id_for_session(1);
    if (!require_true(actor_id.has_value(), "actor id should exist after first tick")) {
        return 1;
    }

    const auto stale = runtime.stage_input(1, server::core::realtime::InputCommand{
        .input_seq = 0,
        .move_x_mm = 999,
    });
    if (!require_true(
            stale.disposition == server::core::realtime::StageInputDisposition::kRejectedStale,
            "older input should be rejected as stale")) {
        return 1;
    }

    const auto staged_3 = runtime.stage_input(1, server::core::realtime::InputCommand{
        .input_seq = 2,
        .move_x_mm = 100,
    });
    const auto staged_4 = runtime.stage_input(2, server::core::realtime::InputCommand{
        .input_seq = 2,
        .move_x_mm = 150,
    });
    if (!require_true(
            staged_3.target_server_tick == 2 && staged_4.target_server_tick == 2,
            "second authoritative update should target tick 2")) {
        return 1;
    }

    const auto second_tick = runtime.tick();
    if (!require_true(second_tick.size() == 2, "second tick should emit one update per viewer")) {
        return 1;
    }
    for (const auto& update : second_tick) {
        if (!require_true(
                update.kind == server::core::realtime::ReplicationKind::kSnapshot,
                "delta budget overflow should fall back to snapshot")) {
            return 1;
        }
    }

    const auto rewind = runtime.rewind_at_or_before(server::core::realtime::RewindQuery{
        .actor_id = *actor_id,
        .server_tick = 2,
    });
    if (!require_true(rewind.has_value(), "rewind query should return a sample")) {
        return 1;
    }
    if (!require_true(rewind->sample.x_mm == 200, "rewind should report accumulated actor position")) {
        return 1;
    }

    server::core::realtime::DirectTransportRolloutPolicy rollout_policy;
    rollout_policy.enabled = true;
    rollout_policy.canary_percent = 100;
    rollout_policy.opcode_allowlist = server::core::realtime::parse_direct_opcode_allowlist("0x0206,0x0208");

    const auto attach = server::core::realtime::evaluate_direct_attach(rollout_policy, "fps-proof", 7);
    if (!require_true(
            attach.mode == server::core::realtime::DirectAttachMode::kRudpCanary,
            "attach evaluation should select RUDP canary")) {
        return 1;
    }
    if (!require_true(
            attach.reason == server::core::realtime::DirectAttachReason::kCanarySelected,
            "attach evaluation should report canary selection")) {
        return 1;
    }

    const auto delivery = server::core::realtime::evaluate_direct_delivery(server::core::realtime::DirectDeliveryContext{
        .direct_path_enabled_for_message = true,
        .udp_bound = true,
        .rudp_selected = true,
        .rudp_established = true,
    });
    if (!require_true(
            delivery.route == server::core::realtime::DirectDeliveryRoute::kRudp,
            "direct delivery should choose RUDP when established")) {
        return 1;
    }
    if (!require_true(
            delivery.reason == server::core::realtime::DirectDeliveryReason::kRudpDirect,
            "direct delivery should report RUDP reason")) {
        return 1;
    }

    server::core::realtime::UdpSequencedMetrics udp_quality;
    (void)udp_quality.on_packet(100, 1000);
    const auto quality = udp_quality.on_packet(102, 1015);
    if (!require_true(quality.accepted, "udp quality tracker should accept forward progress")) {
        return 1;
    }
    if (!require_true(quality.estimated_lost_packets == 1, "udp quality tracker should estimate a missing packet")) {
        return 1;
    }

    const auto bind_request_payload = server::core::realtime::encode_direct_bind_request_payload(server::core::realtime::DirectBindRequest{
        .session_id = "fps-proof",
        .nonce = 9,
        .expires_unix_ms = 1000,
        .token = "token",
    });
    server::core::realtime::DirectBindRequest bind_request{};
    if (!require_true(
            server::core::realtime::decode_direct_bind_request_payload(bind_request_payload, bind_request),
            "bind request payload should decode")) {
        return 1;
    }
    if (!require_true(bind_request.session_id == "fps-proof", "bind request should preserve session id")) {
        return 1;
    }

    const auto bind_response_payload = server::core::realtime::encode_direct_bind_response_payload(
        0,
        server::core::realtime::DirectBindTicket{
            .session_id = "fps-proof",
            .nonce = 9,
            .expires_unix_ms = 1000,
            .token = "token",
        },
        "issued");
    server::core::realtime::DirectBindResponse bind_response{};
    if (!require_true(
            server::core::realtime::decode_direct_bind_response_payload(bind_response_payload, bind_response),
            "bind response payload should decode")) {
        return 1;
    }
    if (!require_true(bind_response.message == "issued", "bind response should preserve message")) {
        return 1;
    }

    const auto snapshot = runtime.snapshot();
    if (!require_true(snapshot.delta_budget_snapshot_total > 0, "runtime snapshot should record delta budget fallback")) {
        return 1;
    }
    if (!require_true(snapshot.stale_input_reject_total > 0, "runtime snapshot should record stale input rejects")) {
        return 1;
    }
    if (!require_true(observer.event_count == 10, "simulation phase observer should see five phases per tick")) {
        return 1;
    }

    return 0;
}

