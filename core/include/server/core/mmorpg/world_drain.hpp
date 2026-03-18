#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "server/core/mmorpg/migration.hpp"
#include "server/core/mmorpg/world_transfer.hpp"

namespace server::core::mmorpg {

/** @brief drain 대상 world에서 관측한 instance 상태 한 건입니다. */
struct ObservedWorldDrainInstance {
    std::string instance_id;
    bool ready{false};
    std::uint32_t active_sessions{0};
};

/** @brief world drain 판단에 필요한 owner/replacement/instance 관측 스냅샷입니다. */
struct ObservedWorldDrainState {
    std::string world_id;
    std::string owner_instance_id;
    bool draining{false};
    std::string replacement_owner_instance_id;
    std::vector<ObservedWorldDrainInstance> instances;
};

enum class WorldDrainPhase : std::uint8_t {
    kIdle = 0,
    kReplacementTargetMissing,
    kReplacementTargetNotReady,
    kDrainingSessions,
    kDrained,
};

inline constexpr std::string_view world_drain_phase_name(WorldDrainPhase phase) noexcept {
    switch (phase) {
    case WorldDrainPhase::kIdle:
        return "idle";
    case WorldDrainPhase::kReplacementTargetMissing:
        return "replacement_target_missing";
    case WorldDrainPhase::kReplacementTargetNotReady:
        return "replacement_target_not_ready";
    case WorldDrainPhase::kDrainingSessions:
        return "draining_sessions";
    case WorldDrainPhase::kDrained:
        return "drained";
    }
    return "idle";
}

/** @brief drain 선언, replacement readiness, active session 분포를 요약합니다. */
struct WorldDrainSummary {
    bool drain_declared{false};
    bool replacement_declared{false};
    bool owner_present{false};
    bool replacement_present{false};
    bool replacement_ready{false};
    std::uint32_t instances_total{0};
    std::uint32_t ready_instances{0};
    std::uint32_t active_sessions_total{0};
    std::uint32_t owner_active_sessions{0};
    std::uint32_t replacement_active_sessions{0};
};

/** @brief 단일 world drain policy의 phase와 요약 상태입니다. */
struct WorldDrainStatus {
    std::string world_id;
    std::string owner_instance_id;
    std::string replacement_owner_instance_id;
    WorldDrainPhase phase{WorldDrainPhase::kIdle};
    WorldDrainSummary summary;
};

enum class WorldDrainOrchestrationPhase : std::uint8_t {
    kIdle = 0,
    kBlockedByReplacementTarget,
    kDraining,
    kAwaitingOwnerTransfer,
    kAwaitingMigration,
    kReadyToClear,
};

inline constexpr std::string_view world_drain_orchestration_phase_name(
    WorldDrainOrchestrationPhase phase) noexcept {
    switch (phase) {
    case WorldDrainOrchestrationPhase::kIdle:
        return "idle";
    case WorldDrainOrchestrationPhase::kBlockedByReplacementTarget:
        return "blocked_by_replacement_target";
    case WorldDrainOrchestrationPhase::kDraining:
        return "draining";
    case WorldDrainOrchestrationPhase::kAwaitingOwnerTransfer:
        return "awaiting_owner_transfer";
    case WorldDrainOrchestrationPhase::kAwaitingMigration:
        return "awaiting_migration";
    case WorldDrainOrchestrationPhase::kReadyToClear:
        return "ready_to_clear";
    }
    return "idle";
}

enum class WorldDrainNextAction : std::uint8_t {
    kNone = 0,
    kWaitForDrain,
    kStabilizeReplacementTarget,
    kCommitOwnerTransfer,
    kAwaitMigration,
    kClearPolicy,
};

inline constexpr std::string_view world_drain_next_action_name(WorldDrainNextAction action) noexcept {
    switch (action) {
    case WorldDrainNextAction::kNone:
        return "none";
    case WorldDrainNextAction::kWaitForDrain:
        return "wait_for_drain";
    case WorldDrainNextAction::kStabilizeReplacementTarget:
        return "stabilize_replacement_target";
    case WorldDrainNextAction::kCommitOwnerTransfer:
        return "commit_owner_transfer";
    case WorldDrainNextAction::kAwaitMigration:
        return "await_migration";
    case WorldDrainNextAction::kClearPolicy:
        return "clear_policy";
    }
    return "none";
}

/** @brief drain, transfer, migration closure 판단에 필요한 orchestration 요약입니다. */
struct WorldDrainOrchestrationSummary {
    bool drain_declared{false};
    bool drained{false};
    bool transfer_declared{false};
    bool transfer_committed{false};
    bool migration_declared{false};
    bool migration_ready{false};
    bool clear_allowed{false};
};

/** @brief drain policy를 언제 clear할 수 있는지 표현하는 orchestration 상태입니다. */
struct WorldDrainOrchestrationStatus {
    std::string world_id;
    WorldDrainPhase drain_phase{WorldDrainPhase::kIdle};
    WorldDrainOrchestrationPhase phase{WorldDrainOrchestrationPhase::kIdle};
    WorldDrainNextAction next_action{WorldDrainNextAction::kNone};
    std::string target_owner_instance_id;
    std::string target_world_id;
    WorldDrainOrchestrationSummary summary;
};

inline WorldDrainStatus evaluate_world_drain(const ObservedWorldDrainState& state) {
    WorldDrainStatus out;
    out.world_id = state.world_id;
    out.owner_instance_id = state.owner_instance_id;
    out.replacement_owner_instance_id = state.replacement_owner_instance_id;
    out.summary.drain_declared = state.draining;
    out.summary.replacement_declared = !state.replacement_owner_instance_id.empty();
    out.summary.owner_present = !state.owner_instance_id.empty();
    out.summary.instances_total = static_cast<std::uint32_t>(state.instances.size());

    for (const auto& instance : state.instances) {
        out.summary.active_sessions_total += instance.active_sessions;
        if (instance.ready) {
            ++out.summary.ready_instances;
        }
        if (!state.owner_instance_id.empty() && instance.instance_id == state.owner_instance_id) {
            out.summary.owner_active_sessions = instance.active_sessions;
        }
        if (!state.replacement_owner_instance_id.empty()
            && instance.instance_id == state.replacement_owner_instance_id) {
            out.summary.replacement_present = true;
            out.summary.replacement_ready = instance.ready;
            out.summary.replacement_active_sessions = instance.active_sessions;
        }
    }

    if (!state.draining) {
        out.phase = WorldDrainPhase::kIdle;
    } else if (out.summary.replacement_declared && !out.summary.replacement_present) {
        out.phase = WorldDrainPhase::kReplacementTargetMissing;
    } else if (out.summary.replacement_declared && !out.summary.replacement_ready) {
        out.phase = WorldDrainPhase::kReplacementTargetNotReady;
    } else if (out.summary.active_sessions_total > 0) {
        out.phase = WorldDrainPhase::kDrainingSessions;
    } else {
        out.phase = WorldDrainPhase::kDrained;
    }

    return out;
}

inline WorldDrainOrchestrationStatus evaluate_world_drain_orchestration(
    const WorldDrainStatus& drain,
    const std::optional<WorldTransferStatus>& transfer = std::nullopt,
    const std::optional<WorldMigrationStatus>& migration = std::nullopt) {
    WorldDrainOrchestrationStatus out;
    out.world_id = drain.world_id;
    out.drain_phase = drain.phase;
    out.summary.drain_declared = drain.summary.drain_declared;
    out.summary.drained = drain.phase == WorldDrainPhase::kDrained;

    if (!drain.summary.drain_declared || drain.phase == WorldDrainPhase::kIdle) {
        out.phase = WorldDrainOrchestrationPhase::kIdle;
        out.next_action = WorldDrainNextAction::kNone;
        return out;
    }

    if (drain.phase == WorldDrainPhase::kReplacementTargetMissing
        || drain.phase == WorldDrainPhase::kReplacementTargetNotReady) {
        out.phase = WorldDrainOrchestrationPhase::kBlockedByReplacementTarget;
        out.next_action = WorldDrainNextAction::kStabilizeReplacementTarget;
        out.target_owner_instance_id = drain.replacement_owner_instance_id;
        return out;
    }

    if (drain.phase == WorldDrainPhase::kDrainingSessions) {
        out.phase = WorldDrainOrchestrationPhase::kDraining;
        out.next_action = WorldDrainNextAction::kWaitForDrain;
        out.target_owner_instance_id = drain.replacement_owner_instance_id;
        return out;
    }

    if (transfer.has_value() && transfer->summary.transfer_declared) {
        out.summary.transfer_declared = true;
        out.summary.transfer_committed =
            transfer->phase == WorldTransferPhase::kOwnerHandoffCommitted;
        out.target_owner_instance_id = transfer->target_owner_instance_id;
        if (!out.summary.transfer_committed) {
            out.phase = WorldDrainOrchestrationPhase::kAwaitingOwnerTransfer;
            out.next_action = WorldDrainNextAction::kCommitOwnerTransfer;
            return out;
        }
    }

    if (migration.has_value() && migration->summary.envelope_present) {
        out.summary.migration_declared = true;
        out.summary.migration_ready =
            migration->phase == WorldMigrationPhase::kReadyToResume;
        out.target_world_id = migration->target_world_id;
        out.target_owner_instance_id = migration->target_owner_instance_id;
        if (!out.summary.migration_ready) {
            out.phase = WorldDrainOrchestrationPhase::kAwaitingMigration;
            out.next_action = WorldDrainNextAction::kAwaitMigration;
            return out;
        }
    }

    out.summary.clear_allowed = true;
    out.phase = WorldDrainOrchestrationPhase::kReadyToClear;
    out.next_action = WorldDrainNextAction::kClearPolicy;
    return out;
}

} // namespace server::core::mmorpg
