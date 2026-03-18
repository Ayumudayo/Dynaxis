#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace server::core::mmorpg {

/** @brief world owner handoff 판단에 필요한 target candidate readiness 한 건입니다. */
struct ObservedWorldTransferInstance {
    std::string instance_id;
    bool ready{false};
};

/** @brief world owner transfer를 계산하기 위한 observed drain/owner snapshot입니다. */
struct ObservedWorldTransferState {
    std::string world_id;
    std::string owner_instance_id;
    bool draining{false};
    std::string replacement_owner_instance_id;
    std::vector<ObservedWorldTransferInstance> instances;
};

enum class WorldTransferPhase : std::uint8_t {
    kIdle = 0,
    kTargetMissing,
    kTargetNotReady,
    kOwnerMissing,
    kAwaitingOwnerHandoff,
    kOwnerHandoffCommitted,
};

inline constexpr std::string_view world_transfer_phase_name(WorldTransferPhase phase) noexcept {
    switch (phase) {
    case WorldTransferPhase::kIdle:
        return "idle";
    case WorldTransferPhase::kTargetMissing:
        return "target_missing";
    case WorldTransferPhase::kTargetNotReady:
        return "target_not_ready";
    case WorldTransferPhase::kOwnerMissing:
        return "owner_missing";
    case WorldTransferPhase::kAwaitingOwnerHandoff:
        return "awaiting_owner_handoff";
    case WorldTransferPhase::kOwnerHandoffCommitted:
        return "owner_handoff_committed";
    }
    return "idle";
}

/** @brief owner transfer의 선언 여부와 target readiness를 요약합니다. */
struct WorldTransferSummary {
    bool transfer_declared{false};
    bool draining{false};
    bool owner_present{false};
    bool target_present{false};
    bool target_ready{false};
    bool owner_matches_target{false};
    std::uint32_t instances_total{0};
    std::uint32_t ready_instances{0};
};

/** @brief world owner handoff contract의 현재 phase입니다. */
struct WorldTransferStatus {
    std::string world_id;
    std::string owner_instance_id;
    std::string target_owner_instance_id;
    WorldTransferPhase phase{WorldTransferPhase::kIdle};
    WorldTransferSummary summary;
};

inline WorldTransferStatus evaluate_world_transfer(const ObservedWorldTransferState& state) {
    WorldTransferStatus out;
    out.world_id = state.world_id;
    out.owner_instance_id = state.owner_instance_id;
    out.target_owner_instance_id = state.replacement_owner_instance_id;
    out.summary.transfer_declared =
        state.draining && !state.replacement_owner_instance_id.empty();
    out.summary.draining = state.draining;
    out.summary.owner_present = !state.owner_instance_id.empty();
    out.summary.instances_total = static_cast<std::uint32_t>(state.instances.size());

    for (const auto& instance : state.instances) {
        if (instance.ready) {
            ++out.summary.ready_instances;
        }
        if (instance.instance_id == state.replacement_owner_instance_id) {
            out.summary.target_present = true;
            out.summary.target_ready = instance.ready;
        }
    }

    out.summary.owner_matches_target =
        out.summary.owner_present
        && !state.replacement_owner_instance_id.empty()
        && state.owner_instance_id == state.replacement_owner_instance_id;

    if (!out.summary.transfer_declared) {
        out.phase = WorldTransferPhase::kIdle;
    } else if (!out.summary.target_present) {
        out.phase = WorldTransferPhase::kTargetMissing;
    } else if (!out.summary.target_ready) {
        out.phase = WorldTransferPhase::kTargetNotReady;
    } else if (!out.summary.owner_present) {
        out.phase = WorldTransferPhase::kOwnerMissing;
    } else if (out.summary.owner_matches_target) {
        out.phase = WorldTransferPhase::kOwnerHandoffCommitted;
    } else {
        out.phase = WorldTransferPhase::kAwaitingOwnerHandoff;
    }

    return out;
}

} // namespace server::core::mmorpg
