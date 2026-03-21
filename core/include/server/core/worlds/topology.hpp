#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace server::core::worlds {

/** @brief 목표 토폴로지(desired topology) 문서에 기록되는 pool replica 목표 한 건입니다. */
struct DesiredTopologyPool {
    std::string world_id;
    std::string shard;
    std::uint32_t replicas{0};
    std::string capacity_class;
    std::vector<std::string> placement_tags;
};

/** @brief 제어면(control plane)이 저장하는 revisioned 목표 토폴로지(desired topology) 문서입니다. */
struct DesiredTopologyDocument {
    std::string topology_id;
    std::uint64_t revision{0};
    std::uint64_t updated_at_ms{0};
    std::vector<DesiredTopologyPool> pools;
};

/** @brief registry heartbeat에서 수집한 인스턴스 단위 관측 토폴로지(observed topology) 값입니다. */
struct ObservedTopologyInstance {
    std::string instance_id;
    std::string role;
    std::string world_id;
    std::string shard;
    bool ready{false};
};

/** @brief world/shard pool 단위로 집계한 관측 토폴로지(observed topology) 상태입니다. */
struct ObservedTopologyPool {
    std::string world_id;
    std::string shard;
    std::uint32_t instances{0};
    std::uint32_t ready_instances{0};
};

enum class TopologyPoolStatus : std::uint8_t {
    kAligned = 0,
    kMissingObservedPool,
    kUnderReplicated,
    kOverReplicated,
    kNoReadyInstances,
    kUndeclaredObservedPool,
};

inline constexpr std::string_view topology_pool_status_name(TopologyPoolStatus status) noexcept {
    switch (status) {
    case TopologyPoolStatus::kAligned:
        return "aligned";
    case TopologyPoolStatus::kMissingObservedPool:
        return "missing_observed_pool";
    case TopologyPoolStatus::kUnderReplicated:
        return "under_replicated";
    case TopologyPoolStatus::kOverReplicated:
        return "over_replicated";
    case TopologyPoolStatus::kNoReadyInstances:
        return "no_ready_instances";
    case TopologyPoolStatus::kUndeclaredObservedPool:
        return "undeclared_observed_pool";
    }
    return "undeclared_observed_pool";
}

/** @brief 목표 pool과 관측 pool을 비교한 조정(reconciliation) 결과 한 건입니다. */
struct ReconciledTopologyPool {
    std::string world_id;
    std::string shard;
    std::uint32_t desired_replicas{0};
    std::uint32_t observed_instances{0};
    std::uint32_t ready_instances{0};
    TopologyPoolStatus status{TopologyPoolStatus::kAligned};
};

/** @brief 전체 desired-vs-observed 조정(reconciliation)의 집계 요약입니다. */
struct TopologyReconciliationSummary {
    bool desired_present{false};
    std::uint32_t desired_pools{0};
    std::uint32_t observed_pools{0};
    std::uint32_t aligned_pools{0};
    std::uint32_t missing_pools{0};
    std::uint32_t under_replicated_pools{0};
    std::uint32_t over_replicated_pools{0};
    std::uint32_t undeclared_pools{0};
    std::uint32_t no_ready_pools{0};
};

/** @brief 목표 토폴로지(desired topology)와 관측 pool 비교의 전체 결과입니다. */
struct TopologyReconciliation {
    TopologyReconciliationSummary summary;
    std::vector<ReconciledTopologyPool> pools;
};

enum class TopologyActuationActionKind : std::uint8_t {
    kScaleOutPool = 0,
    kScaleInPool,
    kRestorePoolReadiness,
    kObserveUndeclaredPool,
};

inline constexpr std::string_view topology_actuation_action_kind_name(
    TopologyActuationActionKind action) noexcept {
    switch (action) {
    case TopologyActuationActionKind::kScaleOutPool:
        return "scale_out_pool";
    case TopologyActuationActionKind::kScaleInPool:
        return "scale_in_pool";
    case TopologyActuationActionKind::kRestorePoolReadiness:
        return "restore_pool_readiness";
    case TopologyActuationActionKind::kObserveUndeclaredPool:
        return "observe_undeclared_pool";
    }
    return "observe_undeclared_pool";
}

inline std::optional<TopologyActuationActionKind> parse_topology_actuation_action_kind(
    std::string_view name) noexcept {
    if (name == "scale_out_pool") {
        return TopologyActuationActionKind::kScaleOutPool;
    }
    if (name == "scale_in_pool") {
        return TopologyActuationActionKind::kScaleInPool;
    }
    if (name == "restore_pool_readiness") {
        return TopologyActuationActionKind::kRestorePoolReadiness;
    }
    if (name == "observe_undeclared_pool") {
        return TopologyActuationActionKind::kObserveUndeclaredPool;
    }
    return std::nullopt;
}

/** @brief 조정(reconciliation) 결과에서 파생된 pool별 operator action 제안 한 건입니다. */
struct TopologyActuationAction {
    std::string world_id;
    std::string shard;
    TopologyPoolStatus status{TopologyPoolStatus::kAligned};
    TopologyActuationActionKind action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::uint32_t desired_replicas{0};
    std::uint32_t observed_instances{0};
    std::uint32_t ready_instances{0};
    std::int32_t replica_delta{0};
    bool actionable{false};
};

/** @brief 읽기 전용 실행 계획(actuation plan)에 포함된 action 종류별 집계입니다. */
struct TopologyActuationPlanSummary {
    bool desired_present{false};
    std::uint32_t actions_total{0};
    std::uint32_t actionable_actions{0};
    std::uint32_t scale_out_actions{0};
    std::uint32_t scale_in_actions{0};
    std::uint32_t readiness_recovery_actions{0};
    std::uint32_t observe_only_actions{0};
};

/** @brief desired/observed mismatch에서 계산한 읽기 전용 실행 계획(actuation plan)입니다. */
struct TopologyActuationPlan {
    TopologyActuationPlanSummary summary;
    std::vector<TopologyActuationAction> actions;
};

/** @brief operator가 승인한 topology action 한 건입니다. */
struct TopologyActuationRequestAction {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::int32_t replica_delta{0};
};

/** @brief operator가 승인한 topology actuation request 문서입니다. */
struct TopologyActuationRequestDocument {
    std::string request_id;
    std::uint64_t revision{0};
    std::uint64_t requested_at_ms{0};
    std::uint64_t basis_topology_revision{0};
    std::vector<TopologyActuationRequestAction> actions;
};

enum class TopologyActuationRequestActionState : std::uint8_t {
    kPending = 0,
    kSatisfied,
    kSuperseded,
};

inline constexpr std::string_view topology_actuation_request_action_state_name(
    TopologyActuationRequestActionState state) noexcept {
    switch (state) {
    case TopologyActuationRequestActionState::kPending:
        return "pending";
    case TopologyActuationRequestActionState::kSatisfied:
        return "satisfied";
    case TopologyActuationRequestActionState::kSuperseded:
        return "superseded";
    }
    return "superseded";
}

/** @brief request action 한 건이 현재 plan에서 pending/satisfied/superseded 중 무엇인지 나타냅니다. */
struct TopologyActuationRequestActionStatus {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind requested_action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::int32_t requested_replica_delta{0};
    TopologyActuationRequestActionState state{TopologyActuationRequestActionState::kPending};
    std::optional<TopologyPoolStatus> current_status;
    std::optional<TopologyActuationActionKind> current_action;
    std::int32_t current_replica_delta{0};
};

/** @brief actuation request가 현재 topology 기준으로 얼마나 아직 유효한지 집계합니다. */
struct TopologyActuationRequestStatusSummary {
    bool request_present{false};
    bool desired_present{false};
    bool basis_topology_revision_matches_current{false};
    std::uint64_t basis_topology_revision{0};
    std::uint64_t current_topology_revision{0};
    std::uint32_t actions_total{0};
    std::uint32_t pending_actions{0};
    std::uint32_t satisfied_actions{0};
    std::uint32_t superseded_actions{0};
};

/** @brief 저장된 request와 현재 topology plan을 비교한 결과입니다. */
struct TopologyActuationRequestStatus {
    TopologyActuationRequestStatusSummary summary;
    std::vector<TopologyActuationRequestActionStatus> actions;
};

/** @brief executor가 claim/complete/fail 대상으로 보는 concrete action 한 건입니다. */
struct TopologyActuationExecutionAction {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::int32_t replica_delta{0};
};

enum class TopologyActuationExecutionActionState : std::uint8_t {
    kClaimed = 0,
    kCompleted,
    kFailed,
};

inline constexpr std::string_view topology_actuation_execution_action_state_name(
    TopologyActuationExecutionActionState state) noexcept {
    switch (state) {
    case TopologyActuationExecutionActionState::kClaimed:
        return "claimed";
    case TopologyActuationExecutionActionState::kCompleted:
        return "completed";
    case TopologyActuationExecutionActionState::kFailed:
        return "failed";
    }
    return "failed";
}

inline std::optional<TopologyActuationExecutionActionState> parse_topology_actuation_execution_action_state(
    std::string_view name) noexcept {
    if (name == "claimed") {
        return TopologyActuationExecutionActionState::kClaimed;
    }
    if (name == "completed") {
        return TopologyActuationExecutionActionState::kCompleted;
    }
    if (name == "failed") {
        return TopologyActuationExecutionActionState::kFailed;
    }
    return std::nullopt;
}

/** @brief execution에서 기준선 observation과 action 상태를 함께 저장하는 항목입니다. */
struct TopologyActuationExecutionItem {
    TopologyActuationExecutionAction action;
    std::uint32_t observed_instances_before{0};
    std::uint32_t ready_instances_before{0};
    TopologyActuationExecutionActionState state{TopologyActuationExecutionActionState::kClaimed};
};

/** @brief executor progress를 기록하는 revisioned execution 문서입니다. */
struct TopologyActuationExecutionDocument {
    std::string executor_id;
    std::uint64_t revision{0};
    std::uint64_t updated_at_ms{0};
    std::uint64_t request_revision{0};
    std::vector<TopologyActuationExecutionItem> actions;
};

enum class TopologyActuationExecutionStatusState : std::uint8_t {
    kAvailable = 0,
    kClaimed,
    kCompleted,
    kFailed,
    kStale,
};

inline constexpr std::string_view topology_actuation_execution_status_state_name(
    TopologyActuationExecutionStatusState state) noexcept {
    switch (state) {
    case TopologyActuationExecutionStatusState::kAvailable:
        return "available";
    case TopologyActuationExecutionStatusState::kClaimed:
        return "claimed";
    case TopologyActuationExecutionStatusState::kCompleted:
        return "completed";
    case TopologyActuationExecutionStatusState::kFailed:
        return "failed";
    case TopologyActuationExecutionStatusState::kStale:
        return "stale";
    }
    return "stale";
}

/** @brief request action 한 건에 대한 현재 execution 상태 해석 결과입니다. */
struct TopologyActuationExecutionActionStatus {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind requested_action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::int32_t requested_replica_delta{0};
    TopologyActuationExecutionStatusState state{TopologyActuationExecutionStatusState::kAvailable};
    std::optional<TopologyActuationRequestActionState> request_state;
    std::optional<TopologyActuationExecutionActionState> execution_state;
};

/** @brief execution 문서가 현재 request와 얼마나 일치하는지 집계합니다. */
struct TopologyActuationExecutionStatusSummary {
    bool request_present{false};
    bool execution_present{false};
    bool execution_revision_matches_current_request{false};
    std::uint64_t execution_request_revision{0};
    std::uint64_t current_request_revision{0};
    std::uint32_t actions_total{0};
    std::uint32_t available_actions{0};
    std::uint32_t claimed_actions{0};
    std::uint32_t completed_actions{0};
    std::uint32_t failed_actions{0};
    std::uint32_t stale_actions{0};
};

/** @brief request 대비 executor claim/completion/failure 상태의 전체 결과입니다. */
struct TopologyActuationExecutionStatus {
    TopologyActuationExecutionStatusSummary summary;
    std::vector<TopologyActuationExecutionActionStatus> actions;
};

enum class TopologyActuationRealizationState : std::uint8_t {
    kAvailable = 0,
    kClaimed,
    kAwaitingObservation,
    kRealized,
    kFailed,
    kStale,
};

inline constexpr std::string_view topology_actuation_realization_state_name(
    TopologyActuationRealizationState state) noexcept {
    switch (state) {
    case TopologyActuationRealizationState::kAvailable:
        return "available";
    case TopologyActuationRealizationState::kClaimed:
        return "claimed";
    case TopologyActuationRealizationState::kAwaitingObservation:
        return "awaiting_observation";
    case TopologyActuationRealizationState::kRealized:
        return "realized";
    case TopologyActuationRealizationState::kFailed:
        return "failed";
    case TopologyActuationRealizationState::kStale:
        return "stale";
    }
    return "stale";
}

/** @brief completed execution이 실제 observation으로 실현됐는지 나타내는 action 상태입니다. */
struct TopologyActuationRealizationActionStatus {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind requested_action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::int32_t requested_replica_delta{0};
    TopologyActuationRealizationState state{TopologyActuationRealizationState::kAvailable};
    std::optional<TopologyActuationRequestActionState> request_state;
    std::optional<TopologyActuationExecutionActionState> execution_state;
    std::uint32_t observed_instances_before{0};
    std::uint32_t ready_instances_before{0};
    std::uint32_t current_observed_instances{0};
    std::uint32_t current_ready_instances{0};
};

/** @brief execution 기준선과 현재 observation 비교 결과를 집계합니다. */
struct TopologyActuationRealizationStatusSummary {
    bool request_present{false};
    bool execution_present{false};
    bool execution_revision_matches_current_request{false};
    std::uint64_t execution_request_revision{0};
    std::uint64_t current_request_revision{0};
    std::uint32_t actions_total{0};
    std::uint32_t available_actions{0};
    std::uint32_t claimed_actions{0};
    std::uint32_t awaiting_observation_actions{0};
    std::uint32_t realized_actions{0};
    std::uint32_t failed_actions{0};
    std::uint32_t stale_actions{0};
};

/** @brief topology action이 observation 상 realized 되었는지 판정한 결과입니다. */
struct TopologyActuationRealizationStatus {
    TopologyActuationRealizationStatusSummary summary;
    std::vector<TopologyActuationRealizationActionStatus> actions;
};

/** @brief scaling adapter가 lease로 받아가는 action 한 건입니다. */
struct TopologyActuationAdapterLeaseAction {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::int32_t replica_delta{0};
};

/** @brief adapter가 claim한 execution slice를 표현하는 lease 문서입니다. */
struct TopologyActuationAdapterLeaseDocument {
    std::string adapter_id;
    std::uint64_t revision{0};
    std::uint64_t leased_at_ms{0};
    std::uint64_t execution_revision{0};
    std::vector<TopologyActuationAdapterLeaseAction> actions;
};

enum class TopologyActuationAdapterStatusState : std::uint8_t {
    kAvailable = 0,
    kLeased,
    kAwaitingRealization,
    kRealized,
    kFailed,
    kStale,
};

inline constexpr std::string_view topology_actuation_adapter_status_state_name(
    TopologyActuationAdapterStatusState state) noexcept {
    switch (state) {
    case TopologyActuationAdapterStatusState::kAvailable:
        return "available";
    case TopologyActuationAdapterStatusState::kLeased:
        return "leased";
    case TopologyActuationAdapterStatusState::kAwaitingRealization:
        return "awaiting_realization";
    case TopologyActuationAdapterStatusState::kRealized:
        return "realized";
    case TopologyActuationAdapterStatusState::kFailed:
        return "failed";
    case TopologyActuationAdapterStatusState::kStale:
        return "stale";
    }
    return "stale";
}

/** @brief adapter lease, execution, realization을 합쳐 action별 adapter 상태를 노출합니다. */
struct TopologyActuationAdapterStatusAction {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind requested_action{TopologyActuationActionKind::kObserveUndeclaredPool};
    std::int32_t requested_replica_delta{0};
    TopologyActuationAdapterStatusState state{TopologyActuationAdapterStatusState::kAvailable};
    std::optional<TopologyActuationExecutionStatusState> execution_state;
    std::optional<TopologyActuationRealizationState> realization_state;
};

/** @brief adapter 관점 lease 상태를 action 종류별로 집계한 요약입니다. */
struct TopologyActuationAdapterStatusSummary {
    bool execution_present{false};
    bool lease_present{false};
    bool lease_revision_matches_current_execution{false};
    std::uint64_t lease_execution_revision{0};
    std::uint64_t current_execution_revision{0};
    std::uint32_t actions_total{0};
    std::uint32_t available_actions{0};
    std::uint32_t leased_actions{0};
    std::uint32_t awaiting_realization_actions{0};
    std::uint32_t realized_actions{0};
    std::uint32_t failed_actions{0};
    std::uint32_t stale_actions{0};
};

/** @brief adapter가 지금 claim/await/realize 가능한 action 전체 상태입니다. */
struct TopologyActuationAdapterStatus {
    TopologyActuationAdapterStatusSummary summary;
    std::vector<TopologyActuationAdapterStatusAction> actions;
};

/** @brief live runtime instance를 특정 world/shard pool로 다시 배치하는 assignment 한 건입니다. */
struct TopologyActuationRuntimeAssignmentItem {
    std::string instance_id;
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind action{TopologyActuationActionKind::kObserveUndeclaredPool};
};

/** @brief runtime process가 polling으로 소비하는 live assignment 문서입니다. */
struct TopologyActuationRuntimeAssignmentDocument {
    std::string adapter_id;
    std::uint64_t revision{0};
    std::uint64_t updated_at_ms{0};
    std::uint64_t lease_revision{0};
    std::vector<TopologyActuationRuntimeAssignmentItem> assignments;
};

inline const TopologyActuationRuntimeAssignmentItem* find_topology_actuation_runtime_assignment(
    const TopologyActuationRuntimeAssignmentDocument& document,
    std::string_view instance_id) noexcept {
    for (const auto& assignment : document.assignments) {
        if (assignment.instance_id == instance_id) {
            return &assignment;
        }
    }
    return nullptr;
}

namespace detail {

inline std::string make_pool_key(std::string_view world_id, std::string_view shard) {
    std::string key;
    key.reserve(world_id.size() + shard.size() + 1);
    key.append(world_id);
    key.push_back('\n');
    key.append(shard);
    return key;
}

} // namespace detail

inline std::vector<ObservedTopologyPool> collect_observed_pools(std::span<const ObservedTopologyInstance> instances) {
    std::unordered_map<std::string, ObservedTopologyPool> pool_index;

    for (const auto& instance : instances) {
        if (instance.role != "server") {
            continue;
        }
        if (instance.world_id.empty() || instance.shard.empty()) {
            continue;
        }

        const std::string key = detail::make_pool_key(instance.world_id, instance.shard);
        auto [it, inserted] = pool_index.emplace(
            key,
            ObservedTopologyPool{
                .world_id = instance.world_id,
                .shard = instance.shard,
            });
        auto& pool = it->second;
        ++pool.instances;
        if (instance.ready) {
            ++pool.ready_instances;
        }
        (void)inserted;
    }

    std::vector<ObservedTopologyPool> pools;
    pools.reserve(pool_index.size());
    for (auto& [_, pool] : pool_index) {
        pools.push_back(std::move(pool));
    }
    return pools;
}

inline TopologyReconciliation reconcile_topology(const std::optional<DesiredTopologyDocument>& desired_topology,
                                                 std::span<const ObservedTopologyPool> observed_pools) {
    TopologyReconciliation out;
    out.summary.desired_present = desired_topology.has_value();
    out.summary.observed_pools = static_cast<std::uint32_t>(observed_pools.size());

    std::unordered_map<std::string, ObservedTopologyPool> observed_index;
    observed_index.reserve(observed_pools.size());
    for (const auto& pool : observed_pools) {
        observed_index.emplace(detail::make_pool_key(pool.world_id, pool.shard), pool);
    }

    if (desired_topology.has_value()) {
        out.summary.desired_pools = static_cast<std::uint32_t>(desired_topology->pools.size());
        for (const auto& desired_pool : desired_topology->pools) {
            ReconciledTopologyPool reconciled;
            reconciled.world_id = desired_pool.world_id;
            reconciled.shard = desired_pool.shard;
            reconciled.desired_replicas = desired_pool.replicas;

            const std::string key = detail::make_pool_key(desired_pool.world_id, desired_pool.shard);
            if (const auto it = observed_index.find(key); it != observed_index.end()) {
                reconciled.observed_instances = it->second.instances;
                reconciled.ready_instances = it->second.ready_instances;

                if (it->second.instances == 0) {
                    reconciled.status = TopologyPoolStatus::kMissingObservedPool;
                    ++out.summary.missing_pools;
                } else if (it->second.ready_instances == 0) {
                    reconciled.status = TopologyPoolStatus::kNoReadyInstances;
                    ++out.summary.no_ready_pools;
                } else if (it->second.instances < desired_pool.replicas) {
                    reconciled.status = TopologyPoolStatus::kUnderReplicated;
                    ++out.summary.under_replicated_pools;
                } else if (it->second.instances > desired_pool.replicas) {
                    reconciled.status = TopologyPoolStatus::kOverReplicated;
                    ++out.summary.over_replicated_pools;
                } else {
                    reconciled.status = TopologyPoolStatus::kAligned;
                    ++out.summary.aligned_pools;
                }

                observed_index.erase(it);
            } else {
                reconciled.status = TopologyPoolStatus::kMissingObservedPool;
                ++out.summary.missing_pools;
            }

            out.pools.push_back(std::move(reconciled));
        }
    }

    for (const auto& [_, observed_pool] : observed_index) {
        ReconciledTopologyPool reconciled;
        reconciled.world_id = observed_pool.world_id;
        reconciled.shard = observed_pool.shard;
        reconciled.observed_instances = observed_pool.instances;
        reconciled.ready_instances = observed_pool.ready_instances;
        reconciled.status = TopologyPoolStatus::kUndeclaredObservedPool;
        ++out.summary.undeclared_pools;
        out.pools.push_back(std::move(reconciled));
    }

    return out;
}

inline TopologyActuationPlan plan_topology_actuation(
    const std::optional<DesiredTopologyDocument>& desired_topology,
    std::span<const ObservedTopologyPool> observed_pools) {
    TopologyActuationPlan out;
    const auto reconciliation = reconcile_topology(desired_topology, observed_pools);
    out.summary.desired_present = reconciliation.summary.desired_present;

    for (const auto& pool : reconciliation.pools) {
        if (pool.status == TopologyPoolStatus::kAligned) {
            continue;
        }

        TopologyActuationAction action;
        action.world_id = pool.world_id;
        action.shard = pool.shard;
        action.status = pool.status;
        action.desired_replicas = pool.desired_replicas;
        action.observed_instances = pool.observed_instances;
        action.ready_instances = pool.ready_instances;

        switch (pool.status) {
        case TopologyPoolStatus::kMissingObservedPool:
        case TopologyPoolStatus::kUnderReplicated:
            action.action = TopologyActuationActionKind::kScaleOutPool;
            action.replica_delta = static_cast<std::int32_t>(pool.desired_replicas)
                - static_cast<std::int32_t>(pool.observed_instances);
            action.actionable = true;
            ++out.summary.scale_out_actions;
            ++out.summary.actionable_actions;
            break;
        case TopologyPoolStatus::kOverReplicated:
            action.action = TopologyActuationActionKind::kScaleInPool;
            action.replica_delta = static_cast<std::int32_t>(pool.observed_instances)
                - static_cast<std::int32_t>(pool.desired_replicas);
            action.actionable = true;
            ++out.summary.scale_in_actions;
            ++out.summary.actionable_actions;
            break;
        case TopologyPoolStatus::kNoReadyInstances:
            action.action = TopologyActuationActionKind::kRestorePoolReadiness;
            action.actionable = true;
            ++out.summary.readiness_recovery_actions;
            ++out.summary.actionable_actions;
            break;
        case TopologyPoolStatus::kUndeclaredObservedPool:
            action.action = TopologyActuationActionKind::kObserveUndeclaredPool;
            action.replica_delta = static_cast<std::int32_t>(pool.observed_instances);
            ++out.summary.observe_only_actions;
            break;
        case TopologyPoolStatus::kAligned:
            break;
        }

        out.actions.push_back(std::move(action));
    }

    out.summary.actions_total = static_cast<std::uint32_t>(out.actions.size());
    return out;
}

inline TopologyActuationRequestStatus evaluate_topology_actuation_request_status(
    const std::optional<TopologyActuationRequestDocument>& request_document,
    const std::optional<DesiredTopologyDocument>& desired_topology,
    std::span<const ObservedTopologyPool> observed_pools) {
    TopologyActuationRequestStatus out;
    out.summary.request_present = request_document.has_value();
    out.summary.desired_present = desired_topology.has_value();
    out.summary.current_topology_revision = desired_topology.has_value() ? desired_topology->revision : 0;

    if (!request_document.has_value()) {
        return out;
    }

    out.summary.basis_topology_revision = request_document->basis_topology_revision;
    out.summary.basis_topology_revision_matches_current =
        desired_topology.has_value() && desired_topology->revision == request_document->basis_topology_revision;

    const auto current_plan = plan_topology_actuation(desired_topology, observed_pools);
    std::unordered_map<std::string, TopologyActuationAction> plan_index;
    plan_index.reserve(current_plan.actions.size());
    for (const auto& action : current_plan.actions) {
        plan_index.emplace(detail::make_pool_key(action.world_id, action.shard), action);
    }

    for (const auto& requested_action : request_document->actions) {
        TopologyActuationRequestActionStatus status;
        status.world_id = requested_action.world_id;
        status.shard = requested_action.shard;
        status.requested_action = requested_action.action;
        status.requested_replica_delta = requested_action.replica_delta;

        const auto key = detail::make_pool_key(requested_action.world_id, requested_action.shard);
        const auto it = plan_index.find(key);
        if (it == plan_index.end()) {
            status.state = TopologyActuationRequestActionState::kSatisfied;
            ++out.summary.satisfied_actions;
        } else {
            status.current_status = it->second.status;
            status.current_action = it->second.action;
            status.current_replica_delta = it->second.replica_delta;
            if (it->second.actionable
                && it->second.action == requested_action.action
                && it->second.replica_delta == requested_action.replica_delta) {
                status.state = TopologyActuationRequestActionState::kPending;
                ++out.summary.pending_actions;
            } else {
                status.state = TopologyActuationRequestActionState::kSuperseded;
                ++out.summary.superseded_actions;
            }
        }

        out.actions.push_back(std::move(status));
    }

    out.summary.actions_total = static_cast<std::uint32_t>(out.actions.size());
    return out;
}

inline TopologyActuationExecutionStatus evaluate_topology_actuation_execution_status(
    const std::optional<TopologyActuationExecutionDocument>& execution_document,
    const std::optional<TopologyActuationRequestDocument>& request_document,
    const std::optional<DesiredTopologyDocument>& desired_topology,
    std::span<const ObservedTopologyPool> observed_pools) {
    TopologyActuationExecutionStatus out;
    out.summary.request_present = request_document.has_value();
    out.summary.execution_present = execution_document.has_value();

    if (execution_document.has_value()) {
        out.summary.execution_request_revision = execution_document->request_revision;
    }
    if (request_document.has_value()) {
        out.summary.current_request_revision = request_document->revision;
    }
    out.summary.execution_revision_matches_current_request =
        execution_document.has_value() && request_document.has_value()
        && execution_document->request_revision == request_document->revision;

    const auto request_status = evaluate_topology_actuation_request_status(
        request_document,
        desired_topology,
        observed_pools);

    std::unordered_map<std::string, TopologyActuationExecutionItem> execution_index;
    if (execution_document.has_value()) {
        execution_index.reserve(execution_document->actions.size());
        for (const auto& item : execution_document->actions) {
            execution_index.emplace(detail::make_pool_key(item.action.world_id, item.action.shard), item);
        }
    }

    for (const auto& request_action : request_status.actions) {
        TopologyActuationExecutionActionStatus status;
        status.world_id = request_action.world_id;
        status.shard = request_action.shard;
        status.requested_action = request_action.requested_action;
        status.requested_replica_delta = request_action.requested_replica_delta;
        status.request_state = request_action.state;

        const auto key = detail::make_pool_key(request_action.world_id, request_action.shard);
        const auto execution_it = execution_index.find(key);
        const bool execution_matches_request =
            execution_it != execution_index.end()
            && execution_it->second.action.action == request_action.requested_action
            && execution_it->second.action.replica_delta == request_action.requested_replica_delta;

        if (request_action.state == TopologyActuationRequestActionState::kPending) {
            if (!execution_document.has_value() || execution_it == execution_index.end()) {
                status.state = TopologyActuationExecutionStatusState::kAvailable;
                ++out.summary.available_actions;
            } else if (!out.summary.execution_revision_matches_current_request || !execution_matches_request) {
                status.execution_state = execution_it->second.state;
                status.state = TopologyActuationExecutionStatusState::kStale;
                ++out.summary.stale_actions;
            } else if (execution_it->second.state == TopologyActuationExecutionActionState::kClaimed) {
                status.execution_state = execution_it->second.state;
                status.state = TopologyActuationExecutionStatusState::kClaimed;
                ++out.summary.claimed_actions;
            } else if (execution_it->second.state == TopologyActuationExecutionActionState::kFailed) {
                status.execution_state = execution_it->second.state;
                status.state = TopologyActuationExecutionStatusState::kFailed;
                ++out.summary.failed_actions;
            } else {
                status.execution_state = execution_it->second.state;
                status.state = TopologyActuationExecutionStatusState::kStale;
                ++out.summary.stale_actions;
            }
        } else if (request_action.state == TopologyActuationRequestActionState::kSatisfied) {
            if (execution_it != execution_index.end()
                && out.summary.execution_revision_matches_current_request
                && execution_matches_request
                && execution_it->second.state == TopologyActuationExecutionActionState::kCompleted) {
                status.execution_state = execution_it->second.state;
                status.state = TopologyActuationExecutionStatusState::kCompleted;
                ++out.summary.completed_actions;
            } else {
                if (execution_it != execution_index.end()) {
                    status.execution_state = execution_it->second.state;
                }
                status.state = TopologyActuationExecutionStatusState::kStale;
                ++out.summary.stale_actions;
            }
        } else {
            if (execution_it != execution_index.end()) {
                status.execution_state = execution_it->second.state;
            }
            status.state = TopologyActuationExecutionStatusState::kStale;
            ++out.summary.stale_actions;
        }

        out.actions.push_back(std::move(status));
    }

    out.summary.actions_total = static_cast<std::uint32_t>(out.actions.size());
    return out;
}

inline TopologyActuationRealizationStatus evaluate_topology_actuation_realization_status(
    const std::optional<TopologyActuationExecutionDocument>& execution_document,
    const std::optional<TopologyActuationRequestDocument>& request_document,
    const std::optional<DesiredTopologyDocument>& desired_topology,
    std::span<const ObservedTopologyPool> observed_pools) {
    TopologyActuationRealizationStatus out;
    const auto execution_status = evaluate_topology_actuation_execution_status(
        execution_document,
        request_document,
        desired_topology,
        observed_pools);
    out.summary.request_present = execution_status.summary.request_present;
    out.summary.execution_present = execution_status.summary.execution_present;
    out.summary.execution_revision_matches_current_request =
        execution_status.summary.execution_revision_matches_current_request;
    out.summary.execution_request_revision = execution_status.summary.execution_request_revision;
    out.summary.current_request_revision = execution_status.summary.current_request_revision;

    std::unordered_map<std::string, TopologyActuationExecutionItem> execution_index;
    if (execution_document.has_value()) {
        execution_index.reserve(execution_document->actions.size());
        for (const auto& item : execution_document->actions) {
            execution_index.emplace(detail::make_pool_key(item.action.world_id, item.action.shard), item);
        }
    }

    std::unordered_map<std::string, ObservedTopologyPool> observed_index;
    observed_index.reserve(observed_pools.size());
    for (const auto& pool : observed_pools) {
        observed_index.emplace(detail::make_pool_key(pool.world_id, pool.shard), pool);
    }

    for (const auto& execution_action : execution_status.actions) {
        TopologyActuationRealizationActionStatus status;
        status.world_id = execution_action.world_id;
        status.shard = execution_action.shard;
        status.requested_action = execution_action.requested_action;
        status.requested_replica_delta = execution_action.requested_replica_delta;
        status.request_state = execution_action.request_state;
        status.execution_state = execution_action.execution_state;

        const auto key = detail::make_pool_key(execution_action.world_id, execution_action.shard);
        if (const auto observed_it = observed_index.find(key); observed_it != observed_index.end()) {
            status.current_observed_instances = observed_it->second.instances;
            status.current_ready_instances = observed_it->second.ready_instances;
        }
        if (const auto execution_it = execution_index.find(key); execution_it != execution_index.end()) {
            status.observed_instances_before = execution_it->second.observed_instances_before;
            status.ready_instances_before = execution_it->second.ready_instances_before;
        }

        switch (execution_action.state) {
        case TopologyActuationExecutionStatusState::kAvailable:
            status.state = TopologyActuationRealizationState::kAvailable;
            ++out.summary.available_actions;
            break;
        case TopologyActuationExecutionStatusState::kClaimed:
            status.state = TopologyActuationRealizationState::kClaimed;
            ++out.summary.claimed_actions;
            break;
        case TopologyActuationExecutionStatusState::kFailed:
            status.state = TopologyActuationRealizationState::kFailed;
            ++out.summary.failed_actions;
            break;
        case TopologyActuationExecutionStatusState::kStale:
            status.state = TopologyActuationRealizationState::kStale;
            ++out.summary.stale_actions;
            break;
        case TopologyActuationExecutionStatusState::kCompleted: {
            bool realized = false;
            switch (execution_action.requested_action) {
            case TopologyActuationActionKind::kScaleOutPool:
                realized = status.current_observed_instances
                    >= status.observed_instances_before
                        + static_cast<std::uint32_t>(std::max(execution_action.requested_replica_delta, 0));
                break;
            case TopologyActuationActionKind::kScaleInPool:
                realized = status.current_observed_instances
                    + static_cast<std::uint32_t>(std::max(execution_action.requested_replica_delta, 0))
                    <= status.observed_instances_before;
                break;
            case TopologyActuationActionKind::kRestorePoolReadiness:
                realized = status.ready_instances_before == 0 && status.current_ready_instances > 0;
                break;
            case TopologyActuationActionKind::kObserveUndeclaredPool:
                realized = false;
                break;
            }
            if (realized) {
                status.state = TopologyActuationRealizationState::kRealized;
                ++out.summary.realized_actions;
            } else {
                status.state = TopologyActuationRealizationState::kAwaitingObservation;
                ++out.summary.awaiting_observation_actions;
            }
            break;
        }
        }

        out.actions.push_back(std::move(status));
    }

    out.summary.actions_total = static_cast<std::uint32_t>(out.actions.size());
    return out;
}

inline TopologyActuationAdapterStatus evaluate_topology_actuation_adapter_status(
    const std::optional<TopologyActuationAdapterLeaseDocument>& lease_document,
    const std::optional<TopologyActuationExecutionDocument>& execution_document,
    const std::optional<TopologyActuationRequestDocument>& request_document,
    const std::optional<DesiredTopologyDocument>& desired_topology,
    std::span<const ObservedTopologyPool> observed_pools) {
    TopologyActuationAdapterStatus out;
    out.summary.execution_present = execution_document.has_value();
    out.summary.lease_present = lease_document.has_value();
    if (lease_document.has_value()) {
        out.summary.lease_execution_revision = lease_document->execution_revision;
    }
    if (execution_document.has_value()) {
        out.summary.current_execution_revision = execution_document->revision;
    }
    out.summary.lease_revision_matches_current_execution =
        lease_document.has_value() && execution_document.has_value()
        && lease_document->execution_revision == execution_document->revision;

    const auto execution_status = evaluate_topology_actuation_execution_status(
        execution_document,
        request_document,
        desired_topology,
        observed_pools);
    const auto realization_status = evaluate_topology_actuation_realization_status(
        execution_document,
        request_document,
        desired_topology,
        observed_pools);

    std::unordered_map<std::string, TopologyActuationAdapterLeaseAction> lease_index;
    if (lease_document.has_value()) {
        lease_index.reserve(lease_document->actions.size());
        for (const auto& action : lease_document->actions) {
            lease_index.emplace(detail::make_pool_key(action.world_id, action.shard), action);
        }
    }

    std::unordered_map<std::string, TopologyActuationRealizationActionStatus> realization_index;
    realization_index.reserve(realization_status.actions.size());
    for (const auto& action : realization_status.actions) {
        realization_index.emplace(detail::make_pool_key(action.world_id, action.shard), action);
    }

    for (const auto& action : execution_status.actions) {
        TopologyActuationAdapterStatusAction status;
        status.world_id = action.world_id;
        status.shard = action.shard;
        status.requested_action = action.requested_action;
        status.requested_replica_delta = action.requested_replica_delta;
        status.execution_state = action.state;

        const auto key = detail::make_pool_key(action.world_id, action.shard);
        const auto lease_it = lease_index.find(key);
        const bool lease_matches_action =
            lease_it != lease_index.end()
            && lease_it->second.action == action.requested_action
            && lease_it->second.replica_delta == action.requested_replica_delta;
        if (const auto realization_it = realization_index.find(key); realization_it != realization_index.end()) {
            status.realization_state = realization_it->second.state;
        }

        if (action.state == TopologyActuationExecutionStatusState::kStale
            || (lease_it != lease_index.end()
                && (!out.summary.lease_revision_matches_current_execution || !lease_matches_action))) {
            status.state = TopologyActuationAdapterStatusState::kStale;
            ++out.summary.stale_actions;
        } else if (status.realization_state == TopologyActuationRealizationState::kRealized) {
            status.state = TopologyActuationAdapterStatusState::kRealized;
            ++out.summary.realized_actions;
        } else if (status.realization_state == TopologyActuationRealizationState::kAwaitingObservation) {
            status.state = lease_it != lease_index.end()
                ? TopologyActuationAdapterStatusState::kAwaitingRealization
                : TopologyActuationAdapterStatusState::kAvailable;
            if (status.state == TopologyActuationAdapterStatusState::kAwaitingRealization) {
                ++out.summary.awaiting_realization_actions;
            } else {
                ++out.summary.available_actions;
            }
        } else if (action.state == TopologyActuationExecutionStatusState::kFailed) {
            status.state = lease_it != lease_index.end()
                ? TopologyActuationAdapterStatusState::kFailed
                : TopologyActuationAdapterStatusState::kAvailable;
            if (status.state == TopologyActuationAdapterStatusState::kFailed) {
                ++out.summary.failed_actions;
            } else {
                ++out.summary.available_actions;
            }
        } else if (lease_it != lease_index.end() && lease_matches_action) {
            status.state = TopologyActuationAdapterStatusState::kLeased;
            ++out.summary.leased_actions;
        } else {
            status.state = TopologyActuationAdapterStatusState::kAvailable;
            ++out.summary.available_actions;
        }

        out.actions.push_back(std::move(status));
    }

    out.summary.actions_total = static_cast<std::uint32_t>(out.actions.size());
    return out;
}

} // namespace server::core::worlds

