#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "server/core/worlds/topology.hpp"
#include "server/core/worlds/world_drain.hpp"

namespace server::core::worlds {

/** @brief Kubernetes 쪽에서 topology pool을 어떤 workload primitive로 다룰지 나타냅니다. */
enum class KubernetesWorkloadKind : std::uint8_t {
    kDeployment = 0,
    kStatefulSet,
};

inline constexpr std::string_view kubernetes_workload_kind_name(
    KubernetesWorkloadKind kind) noexcept {
    switch (kind) {
    case KubernetesWorkloadKind::kDeployment:
        return "deployment";
    case KubernetesWorkloadKind::kStatefulSet:
        return "statefulset";
    }
    return "statefulset";
}

inline std::optional<KubernetesWorkloadKind> parse_kubernetes_workload_kind(
    std::string_view value) noexcept {
    if (value == "deployment") {
        return KubernetesWorkloadKind::kDeployment;
    }
    if (value == "statefulset") {
        return KubernetesWorkloadKind::kStatefulSet;
    }
    return std::nullopt;
}

/** @brief desired topology pool 한 건을 Kubernetes namespace/workload에 연결한 바인딩입니다. */
struct KubernetesPoolBinding {
    std::string world_id;
    std::string shard;
    std::string namespace_name;
    std::string workload_name;
    KubernetesWorkloadKind workload_kind{KubernetesWorkloadKind::kStatefulSet};
    std::uint32_t target_replicas{0};
    std::string capacity_class;
    std::vector<std::string> placement_tags;
};

/** @brief Kubernetes workload와 runtime 측의 현재 관측 카운터입니다. */
struct KubernetesPoolObservation {
    std::uint32_t current_spec_replicas{0};
    std::uint32_t ready_replicas{0};
    std::uint32_t available_replicas{0};
    std::uint32_t terminating_replicas{0};
    std::uint32_t assigned_runtime_instances{0};
    std::uint32_t idle_ready_runtime_instances{0};
};

enum class KubernetesPoolOrchestrationPhase : std::uint8_t {
    kIdle = 0,
    kScaleWorkload,
    kAwaitReadyReplicas,
    kAwaitRuntimeAssignment,
    kAwaitReplacementTarget,
    kAwaitDrain,
    kAwaitOwnerTransfer,
    kAwaitMigration,
    kRetireWorkload,
    kComplete,
    kStale,
};

inline constexpr std::string_view kubernetes_pool_orchestration_phase_name(
    KubernetesPoolOrchestrationPhase phase) noexcept {
    switch (phase) {
    case KubernetesPoolOrchestrationPhase::kIdle:
        return "idle";
    case KubernetesPoolOrchestrationPhase::kScaleWorkload:
        return "scale_workload";
    case KubernetesPoolOrchestrationPhase::kAwaitReadyReplicas:
        return "await_ready_replicas";
    case KubernetesPoolOrchestrationPhase::kAwaitRuntimeAssignment:
        return "await_runtime_assignment";
    case KubernetesPoolOrchestrationPhase::kAwaitReplacementTarget:
        return "await_replacement_target";
    case KubernetesPoolOrchestrationPhase::kAwaitDrain:
        return "await_drain";
    case KubernetesPoolOrchestrationPhase::kAwaitOwnerTransfer:
        return "await_owner_transfer";
    case KubernetesPoolOrchestrationPhase::kAwaitMigration:
        return "await_migration";
    case KubernetesPoolOrchestrationPhase::kRetireWorkload:
        return "retire_workload";
    case KubernetesPoolOrchestrationPhase::kComplete:
        return "complete";
    case KubernetesPoolOrchestrationPhase::kStale:
        return "stale";
    }
    return "stale";
}

enum class KubernetesPoolNextAction : std::uint8_t {
    kNone = 0,
    kPatchWorkloadReplicas,
    kWaitForPodsReady,
    kPublishRuntimeAssignments,
    kStabilizeReplacementTarget,
    kWaitForDrain,
    kCommitOwnerTransfer,
    kWaitForMigration,
    kPatchWorkloadRetirement,
};

inline constexpr std::string_view kubernetes_pool_next_action_name(
    KubernetesPoolNextAction action) noexcept {
    switch (action) {
    case KubernetesPoolNextAction::kNone:
        return "none";
    case KubernetesPoolNextAction::kPatchWorkloadReplicas:
        return "patch_workload_replicas";
    case KubernetesPoolNextAction::kWaitForPodsReady:
        return "wait_for_pods_ready";
    case KubernetesPoolNextAction::kPublishRuntimeAssignments:
        return "publish_runtime_assignments";
    case KubernetesPoolNextAction::kStabilizeReplacementTarget:
        return "stabilize_replacement_target";
    case KubernetesPoolNextAction::kWaitForDrain:
        return "wait_for_drain";
    case KubernetesPoolNextAction::kCommitOwnerTransfer:
        return "commit_owner_transfer";
    case KubernetesPoolNextAction::kWaitForMigration:
        return "wait_for_migration";
    case KubernetesPoolNextAction::kPatchWorkloadRetirement:
        return "patch_workload_retirement";
    }
    return "none";
}

/**
 * @brief Kubernetes-first controller가 현재 pool을 어떤 단계로 보고 있는지 요약합니다.
 *
 * 이 요약은 단순 replica 수만 보는 것이 아니라, runtime assignment와 drain orchestration까지
 * 함께 포함합니다. 그래야 "pod는 떴지만 실제 world handoff는 끝나지 않은 상태"를 따로 표현할 수 있습니다.
 */
struct KubernetesPoolOrchestrationSummary {
    bool action_present{false};
    bool assignment_present{false};
    bool binding_target_reached{false};
    bool pods_ready_for_target{false};
    bool runtime_assignment_satisfied{false};
    bool drain_ready_to_retire{false};
    std::uint32_t target_replicas{0};
    std::uint32_t current_spec_replicas{0};
    std::uint32_t ready_replicas{0};
    std::uint32_t available_replicas{0};
    std::uint32_t terminating_replicas{0};
    std::uint32_t assigned_runtime_instances{0};
    std::uint32_t idle_ready_runtime_instances{0};
    WorldDrainOrchestrationPhase drain_orchestration_phase{WorldDrainOrchestrationPhase::kIdle};
};

/**
 * @brief topology actuation을 Kubernetes workload lifecycle vocabulary로 해석한 결과입니다.
 *
 * core가 직접 Kubernetes API를 호출하지 않더라도, control-plane은 "지금 이 pool에서 다음에
 * 무엇을 해야 하는가"를 Kubernetes 언어로 읽을 수 있어야 합니다. 이 구조체가 그 번역 결과를 담습니다.
 */
struct KubernetesPoolOrchestrationStatus {
    std::string world_id;
    std::string shard;
    std::string namespace_name;
    std::string workload_name;
    KubernetesWorkloadKind workload_kind{KubernetesWorkloadKind::kStatefulSet};
    TopologyActuationActionKind requested_action{TopologyActuationActionKind::kObserveUndeclaredPool};
    KubernetesPoolOrchestrationPhase phase{KubernetesPoolOrchestrationPhase::kIdle};
    KubernetesPoolNextAction next_action{KubernetesPoolNextAction::kNone};
    std::string target_owner_instance_id;
    std::string target_world_id;
    KubernetesPoolOrchestrationSummary summary;
};

inline std::string sanitize_kubernetes_name_token(std::string_view value) {
    std::string out;
    out.reserve(value.size());

    for (const char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0) {
            out.push_back(static_cast<char>(std::tolower(uch)));
        } else {
            out.push_back('-');
        }
    }

    std::string collapsed;
    collapsed.reserve(out.size());
    bool previous_dash = false;
    for (const char ch : out) {
        if (ch == '-') {
            if (!previous_dash) {
                collapsed.push_back(ch);
            }
            previous_dash = true;
            continue;
        }
        collapsed.push_back(ch);
        previous_dash = false;
    }

    while (!collapsed.empty() && collapsed.front() == '-') {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && collapsed.back() == '-') {
        collapsed.pop_back();
    }

    if (collapsed.empty()) {
        collapsed = "pool";
    }

    return collapsed;
}

inline std::string make_kubernetes_pool_workload_name(
    std::string_view world_id,
    std::string_view shard,
    KubernetesWorkloadKind workload_kind = KubernetesWorkloadKind::kStatefulSet) {
    const std::string prefix = workload_kind == KubernetesWorkloadKind::kStatefulSet
        ? "world-set-"
        : "world-deploy-";
    return prefix
        + sanitize_kubernetes_name_token(world_id)
        + "-"
        + sanitize_kubernetes_name_token(shard);
}

inline KubernetesPoolBinding make_kubernetes_pool_binding(
    const DesiredTopologyPool& pool,
    std::string_view namespace_name = "dynaxis-worlds",
    KubernetesWorkloadKind workload_kind = KubernetesWorkloadKind::kStatefulSet) {
    KubernetesPoolBinding binding;
    binding.world_id = pool.world_id;
    binding.shard = pool.shard;
    binding.namespace_name = std::string(namespace_name);
    binding.workload_name = make_kubernetes_pool_workload_name(pool.world_id, pool.shard, workload_kind);
    binding.workload_kind = workload_kind;
    binding.target_replicas = pool.replicas;
    binding.capacity_class = pool.capacity_class;
    binding.placement_tags = pool.placement_tags;
    return binding;
}

inline std::uint32_t count_topology_actuation_runtime_assignments(
    const TopologyActuationRuntimeAssignmentDocument& document,
    std::string_view world_id,
    std::string_view shard,
    TopologyActuationActionKind action) noexcept {
    std::uint32_t count = 0;
    for (const auto& assignment : document.assignments) {
        if (assignment.world_id == world_id
            && assignment.shard == shard
            && assignment.action == action) {
            ++count;
        }
    }
    return count;
}

inline KubernetesPoolOrchestrationStatus evaluate_kubernetes_pool_orchestration(
    const KubernetesPoolBinding& binding,
    const KubernetesPoolObservation& observation,
    const std::optional<TopologyActuationAdapterLeaseAction>& lease_action = std::nullopt,
    const std::optional<WorldDrainOrchestrationStatus>& drain_orchestration = std::nullopt) {
    KubernetesPoolOrchestrationStatus out;
    out.world_id = binding.world_id;
    out.shard = binding.shard;
    out.namespace_name = binding.namespace_name;
    out.workload_name = binding.workload_name;
    out.workload_kind = binding.workload_kind;
    out.summary.action_present = lease_action.has_value();
    out.summary.assignment_present = observation.assigned_runtime_instances > 0;
    out.summary.target_replicas = binding.target_replicas;
    out.summary.current_spec_replicas = observation.current_spec_replicas;
    out.summary.ready_replicas = observation.ready_replicas;
    out.summary.available_replicas = observation.available_replicas;
    out.summary.terminating_replicas = observation.terminating_replicas;
    out.summary.assigned_runtime_instances = observation.assigned_runtime_instances;
    out.summary.idle_ready_runtime_instances = observation.idle_ready_runtime_instances;
    out.summary.binding_target_reached = observation.current_spec_replicas == binding.target_replicas;
    out.summary.pods_ready_for_target = observation.ready_replicas >= binding.target_replicas;
    out.summary.drain_ready_to_retire =
        drain_orchestration.has_value()
        && drain_orchestration->phase == WorldDrainOrchestrationPhase::kReadyToClear;
    if (drain_orchestration.has_value()) {
        out.summary.drain_orchestration_phase = drain_orchestration->phase;
        out.target_owner_instance_id = drain_orchestration->target_owner_instance_id;
        out.target_world_id = drain_orchestration->target_world_id;
    }

    if (!lease_action.has_value()) {
        out.phase = KubernetesPoolOrchestrationPhase::kIdle;
        out.next_action = KubernetesPoolNextAction::kNone;
        return out;
    }

    out.requested_action = lease_action->action;

    switch (lease_action->action) {
    case TopologyActuationActionKind::kScaleOutPool: {
        const auto required_assignments = static_cast<std::uint32_t>(
            std::max(lease_action->replica_delta, 0));
        out.summary.runtime_assignment_satisfied =
            observation.assigned_runtime_instances >= required_assignments;

        if (observation.current_spec_replicas < binding.target_replicas) {
            out.phase = KubernetesPoolOrchestrationPhase::kScaleWorkload;
            out.next_action = KubernetesPoolNextAction::kPatchWorkloadReplicas;
        } else if (observation.ready_replicas < binding.target_replicas) {
            out.phase = KubernetesPoolOrchestrationPhase::kAwaitReadyReplicas;
            out.next_action = KubernetesPoolNextAction::kWaitForPodsReady;
        } else if (observation.assigned_runtime_instances < required_assignments) {
            out.phase = KubernetesPoolOrchestrationPhase::kAwaitRuntimeAssignment;
            out.next_action = KubernetesPoolNextAction::kPublishRuntimeAssignments;
        } else {
            out.phase = KubernetesPoolOrchestrationPhase::kComplete;
            out.next_action = KubernetesPoolNextAction::kNone;
        }
        return out;
    }
    case TopologyActuationActionKind::kRestorePoolReadiness:
        out.summary.runtime_assignment_satisfied = true;
        if (observation.current_spec_replicas < binding.target_replicas) {
            out.phase = KubernetesPoolOrchestrationPhase::kScaleWorkload;
            out.next_action = KubernetesPoolNextAction::kPatchWorkloadReplicas;
        } else if (observation.ready_replicas == 0) {
            out.phase = KubernetesPoolOrchestrationPhase::kAwaitReadyReplicas;
            out.next_action = KubernetesPoolNextAction::kWaitForPodsReady;
        } else {
            out.phase = KubernetesPoolOrchestrationPhase::kComplete;
            out.next_action = KubernetesPoolNextAction::kNone;
        }
        return out;
    case TopologyActuationActionKind::kScaleInPool:
        out.summary.runtime_assignment_satisfied = true;
        if (drain_orchestration.has_value()) {
            switch (drain_orchestration->phase) {
            case WorldDrainOrchestrationPhase::kBlockedByReplacementTarget:
                out.phase = KubernetesPoolOrchestrationPhase::kAwaitReplacementTarget;
                out.next_action = KubernetesPoolNextAction::kStabilizeReplacementTarget;
                return out;
            case WorldDrainOrchestrationPhase::kDraining:
                out.phase = KubernetesPoolOrchestrationPhase::kAwaitDrain;
                out.next_action = KubernetesPoolNextAction::kWaitForDrain;
                return out;
            case WorldDrainOrchestrationPhase::kAwaitingOwnerTransfer:
                out.phase = KubernetesPoolOrchestrationPhase::kAwaitOwnerTransfer;
                out.next_action = KubernetesPoolNextAction::kCommitOwnerTransfer;
                return out;
            case WorldDrainOrchestrationPhase::kAwaitingMigration:
                out.phase = KubernetesPoolOrchestrationPhase::kAwaitMigration;
                out.next_action = KubernetesPoolNextAction::kWaitForMigration;
                return out;
            case WorldDrainOrchestrationPhase::kReadyToClear:
            case WorldDrainOrchestrationPhase::kIdle:
                break;
            }
        }

        if (observation.current_spec_replicas > binding.target_replicas
            || observation.terminating_replicas > 0) {
            out.phase = KubernetesPoolOrchestrationPhase::kRetireWorkload;
            out.next_action = KubernetesPoolNextAction::kPatchWorkloadRetirement;
        } else {
            out.phase = KubernetesPoolOrchestrationPhase::kComplete;
            out.next_action = KubernetesPoolNextAction::kNone;
        }
        return out;
    case TopologyActuationActionKind::kObserveUndeclaredPool:
        out.summary.runtime_assignment_satisfied = true;
        out.phase = KubernetesPoolOrchestrationPhase::kIdle;
        out.next_action = KubernetesPoolNextAction::kNone;
        return out;
    }

    out.phase = KubernetesPoolOrchestrationPhase::kStale;
    out.next_action = KubernetesPoolNextAction::kNone;
    return out;
}

} // namespace server::core::worlds
