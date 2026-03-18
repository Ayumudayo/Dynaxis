#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include <server/core/worlds/topology.hpp>

TEST(MmorpgTopologyContractTest, CollectsObservedPoolsByWorldAndShard) {
    const std::vector<server::core::worlds::ObservedTopologyInstance> instances{
        {.instance_id = "server-1", .role = "server", .world_id = "starter-a", .shard = "alpha", .ready = true},
        {.instance_id = "server-2", .role = "server", .world_id = "starter-a", .shard = "alpha", .ready = false},
        {.instance_id = "server-3", .role = "server", .world_id = "starter-b", .shard = "beta", .ready = true},
        {.instance_id = "gateway-1", .role = "gateway", .world_id = "starter-a", .shard = "alpha", .ready = true},
    };

    const auto pools = server::core::worlds::collect_observed_pools(instances);
    ASSERT_EQ(pools.size(), 2u);

    const auto first = std::find_if(pools.begin(), pools.end(), [](const auto& pool) {
        return pool.world_id == "starter-a" && pool.shard == "alpha";
    });
    ASSERT_NE(first, pools.end());
    EXPECT_EQ(first->instances, 2u);
    EXPECT_EQ(first->ready_instances, 1u);
}

TEST(MmorpgTopologyContractTest, ReconcilesDesiredAndObservedPools) {
    server::core::worlds::DesiredTopologyDocument desired;
    desired.topology_id = "starter";
    desired.revision = 2;
    desired.pools = {
        {.world_id = "starter-a", .shard = "alpha", .replicas = 2},
        {.world_id = "starter-b", .shard = "beta", .replicas = 1},
    };

    const std::vector<server::core::worlds::ObservedTopologyPool> observed{
        {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-c", .shard = "gamma", .instances = 1, .ready_instances = 0},
    };

    const auto reconciliation = server::core::worlds::reconcile_topology(desired, observed);
    EXPECT_TRUE(reconciliation.summary.desired_present);
    EXPECT_EQ(reconciliation.summary.desired_pools, 2u);
    EXPECT_EQ(reconciliation.summary.observed_pools, 2u);
    EXPECT_EQ(reconciliation.summary.under_replicated_pools, 1u);
    EXPECT_EQ(reconciliation.summary.missing_pools, 1u);
    EXPECT_EQ(reconciliation.summary.undeclared_pools, 1u);

    const auto under_replicated = std::find_if(reconciliation.pools.begin(), reconciliation.pools.end(), [](const auto& pool) {
        return pool.world_id == "starter-a";
    });
    ASSERT_NE(under_replicated, reconciliation.pools.end());
    EXPECT_EQ(under_replicated->status, server::core::worlds::TopologyPoolStatus::kUnderReplicated);

    const auto undeclared = std::find_if(reconciliation.pools.begin(), reconciliation.pools.end(), [](const auto& pool) {
        return pool.world_id == "starter-c";
    });
    ASSERT_NE(undeclared, reconciliation.pools.end());
    EXPECT_EQ(undeclared->status, server::core::worlds::TopologyPoolStatus::kUndeclaredObservedPool);
}

TEST(MmorpgTopologyContractTest, PlansReadOnlyTopologyActuationActionsFromReconciliationStatus) {
    server::core::worlds::DesiredTopologyDocument desired;
    desired.topology_id = "starter";
    desired.revision = 2;
    desired.pools = {
        {.world_id = "starter-a", .shard = "alpha", .replicas = 2},
        {.world_id = "starter-b", .shard = "beta", .replicas = 1},
    };

    const std::vector<server::core::worlds::ObservedTopologyPool> observed{
        {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-b", .shard = "beta", .instances = 1, .ready_instances = 0},
        {.world_id = "starter-c", .shard = "gamma", .instances = 1, .ready_instances = 1},
    };

    const auto plan = server::core::worlds::plan_topology_actuation(desired, observed);
    EXPECT_TRUE(plan.summary.desired_present);
    EXPECT_EQ(plan.summary.actions_total, 3u);
    EXPECT_EQ(plan.summary.actionable_actions, 2u);
    EXPECT_EQ(plan.summary.scale_out_actions, 1u);
    EXPECT_EQ(plan.summary.readiness_recovery_actions, 1u);
    EXPECT_EQ(plan.summary.observe_only_actions, 1u);

    const auto scale_out = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& action) {
        return action.world_id == "starter-a";
    });
    ASSERT_NE(scale_out, plan.actions.end());
    EXPECT_EQ(scale_out->action, server::core::worlds::TopologyActuationActionKind::kScaleOutPool);
    EXPECT_EQ(scale_out->replica_delta, 1);
    EXPECT_TRUE(scale_out->actionable);

    const auto restore = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& action) {
        return action.world_id == "starter-b";
    });
    ASSERT_NE(restore, plan.actions.end());
    EXPECT_EQ(restore->action, server::core::worlds::TopologyActuationActionKind::kRestorePoolReadiness);
    EXPECT_TRUE(restore->actionable);

    const auto observe_only = std::find_if(plan.actions.begin(), plan.actions.end(), [](const auto& action) {
        return action.world_id == "starter-c";
    });
    ASSERT_NE(observe_only, plan.actions.end());
    EXPECT_EQ(
        observe_only->action,
        server::core::worlds::TopologyActuationActionKind::kObserveUndeclaredPool);
    EXPECT_FALSE(observe_only->actionable);
}

TEST(MmorpgTopologyContractTest, EvaluatesActuationRequestStatusAgainstCurrentPlan) {
    server::core::worlds::DesiredTopologyDocument desired;
    desired.topology_id = "starter";
    desired.revision = 4;
    desired.pools = {
        {.world_id = "starter-a", .shard = "alpha", .replicas = 3},
        {.world_id = "starter-b", .shard = "beta", .replicas = 1},
        {.world_id = "starter-d", .shard = "delta", .replicas = 2},
    };

    const std::vector<server::core::worlds::ObservedTopologyPool> observed{
        {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-b", .shard = "beta", .instances = 2, .ready_instances = 1},
    };

    server::core::worlds::TopologyActuationRequestDocument request_document;
    request_document.request_id = "scale-starter-a";
    request_document.basis_topology_revision = 3;
    request_document.actions = {
        {
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 2,
        },
        {
            .world_id = "starter-d",
            .shard = "delta",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 1,
        },
        {
            .world_id = "starter-c",
            .shard = "gamma",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 1,
        },
    };

    const auto status = server::core::worlds::evaluate_topology_actuation_request_status(
        request_document,
        desired,
        observed);
    EXPECT_TRUE(status.summary.request_present);
    EXPECT_TRUE(status.summary.desired_present);
    EXPECT_FALSE(status.summary.basis_topology_revision_matches_current);
    EXPECT_EQ(status.summary.current_topology_revision, 4u);
    EXPECT_EQ(status.summary.actions_total, 3u);
    EXPECT_EQ(status.summary.pending_actions, 1u);
    EXPECT_EQ(status.summary.superseded_actions, 1u);
    EXPECT_EQ(status.summary.satisfied_actions, 1u);

    const auto pending = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-a";
    });
    ASSERT_NE(pending, status.actions.end());
    EXPECT_EQ(pending->state, server::core::worlds::TopologyActuationRequestActionState::kPending);
    ASSERT_TRUE(pending->current_action.has_value());
    EXPECT_EQ(*pending->current_action, server::core::worlds::TopologyActuationActionKind::kScaleOutPool);
    EXPECT_EQ(pending->current_replica_delta, 2);

    const auto superseded = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-d";
    });
    ASSERT_NE(superseded, status.actions.end());
    EXPECT_EQ(superseded->state, server::core::worlds::TopologyActuationRequestActionState::kSuperseded);
    ASSERT_TRUE(superseded->current_action.has_value());
    EXPECT_EQ(*superseded->current_action, server::core::worlds::TopologyActuationActionKind::kScaleOutPool);

    const auto satisfied = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-c";
    });
    ASSERT_NE(satisfied, status.actions.end());
    EXPECT_EQ(satisfied->state, server::core::worlds::TopologyActuationRequestActionState::kSatisfied);
    EXPECT_FALSE(satisfied->current_action.has_value());
}

TEST(MmorpgTopologyContractTest, EvaluatesActuationExecutionStatusAgainstRequestAndPlan) {
    server::core::worlds::DesiredTopologyDocument desired;
    desired.topology_id = "starter";
    desired.revision = 9;
    desired.pools = {
        {.world_id = "starter-a", .shard = "alpha", .replicas = 3},
        {.world_id = "starter-b", .shard = "beta", .replicas = 4},
        {.world_id = "starter-c", .shard = "gamma", .replicas = 5},
        {.world_id = "starter-d", .shard = "delta", .replicas = 1},
        {.world_id = "starter-e", .shard = "epsilon", .replicas = 2},
    };

    const std::vector<server::core::worlds::ObservedTopologyPool> observed{
        {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-b", .shard = "beta", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-c", .shard = "gamma", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-d", .shard = "delta", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-e", .shard = "epsilon", .instances = 1, .ready_instances = 1},
    };

    server::core::worlds::TopologyActuationRequestDocument request_document;
    request_document.request_id = "executor-boundary";
    request_document.revision = 4;
    request_document.basis_topology_revision = 9;
    request_document.actions = {
        {.world_id = "starter-a", .shard = "alpha", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
        {.world_id = "starter-b", .shard = "beta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 3},
        {.world_id = "starter-c", .shard = "gamma", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 4},
        {.world_id = "starter-d", .shard = "delta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
        {.world_id = "starter-e", .shard = "epsilon", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
    };

    server::core::worlds::TopologyActuationExecutionDocument execution_document;
    execution_document.executor_id = "executor-a";
    execution_document.request_revision = 4;
    execution_document.actions = {
        {
            .action = {.world_id = "starter-b", .shard = "beta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 3},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kClaimed,
        },
        {
            .action = {.world_id = "starter-c", .shard = "gamma", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 4},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kFailed,
        },
        {
            .action = {.world_id = "starter-d", .shard = "delta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kCompleted,
        },
    };

    const auto request_status = server::core::worlds::evaluate_topology_actuation_request_status(
        request_document,
        desired,
        observed);
    const auto request_satisfied = std::find_if(request_status.actions.begin(), request_status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-d";
    });
    ASSERT_NE(request_satisfied, request_status.actions.end());
    EXPECT_EQ(request_satisfied->state, server::core::worlds::TopologyActuationRequestActionState::kSatisfied);

    const auto status = server::core::worlds::evaluate_topology_actuation_execution_status(
        execution_document,
        request_document,
        desired,
        observed);
    EXPECT_TRUE(status.summary.request_present);
    EXPECT_TRUE(status.summary.execution_present);
    EXPECT_TRUE(status.summary.execution_revision_matches_current_request);
    EXPECT_EQ(status.summary.actions_total, 5u);
    EXPECT_EQ(status.summary.available_actions, 1u);
    EXPECT_EQ(status.summary.claimed_actions, 1u);
    EXPECT_EQ(status.summary.completed_actions, 1u);
    EXPECT_EQ(status.summary.failed_actions, 1u);
    EXPECT_EQ(status.summary.stale_actions, 1u);

    const auto available = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-a";
    });
    ASSERT_NE(available, status.actions.end());
    EXPECT_EQ(available->state, server::core::worlds::TopologyActuationExecutionStatusState::kAvailable);

    const auto claimed = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-b";
    });
    ASSERT_NE(claimed, status.actions.end());
    EXPECT_EQ(claimed->state, server::core::worlds::TopologyActuationExecutionStatusState::kClaimed);

    const auto failed = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-c";
    });
    ASSERT_NE(failed, status.actions.end());
    EXPECT_EQ(failed->state, server::core::worlds::TopologyActuationExecutionStatusState::kFailed);

    const auto completed = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-d";
    });
    ASSERT_NE(completed, status.actions.end());
    EXPECT_EQ(completed->state, server::core::worlds::TopologyActuationExecutionStatusState::kCompleted);

    const auto stale = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-e";
    });
    ASSERT_NE(stale, status.actions.end());
    EXPECT_EQ(stale->state, server::core::worlds::TopologyActuationExecutionStatusState::kStale);
}

TEST(MmorpgTopologyContractTest, EvaluatesActuationRealizationStatusFromObservedDelta) {
    server::core::worlds::DesiredTopologyDocument desired;
    desired.topology_id = "starter";
    desired.revision = 10;
    desired.pools = {
        {.world_id = "starter-a", .shard = "alpha", .replicas = 3},
        {.world_id = "starter-b", .shard = "beta", .replicas = 4},
        {.world_id = "starter-c", .shard = "gamma", .replicas = 5},
        {.world_id = "starter-d", .shard = "delta", .replicas = 2},
        {.world_id = "starter-e", .shard = "epsilon", .replicas = 2},
    };

    const std::vector<server::core::worlds::ObservedTopologyPool> observed{
        {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-b", .shard = "beta", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-c", .shard = "gamma", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-d", .shard = "delta", .instances = 2, .ready_instances = 1},
        {.world_id = "starter-e", .shard = "epsilon", .instances = 1, .ready_instances = 1},
    };

    server::core::worlds::TopologyActuationRequestDocument request_document;
    request_document.request_id = "realization-boundary";
    request_document.revision = 5;
    request_document.basis_topology_revision = 10;
    request_document.actions = {
        {.world_id = "starter-a", .shard = "alpha", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
        {.world_id = "starter-b", .shard = "beta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 3},
        {.world_id = "starter-c", .shard = "gamma", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 4},
        {.world_id = "starter-d", .shard = "delta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
        {.world_id = "starter-e", .shard = "epsilon", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
    };

    server::core::worlds::TopologyActuationExecutionDocument execution_document;
    execution_document.executor_id = "executor-a";
    execution_document.request_revision = 5;
    execution_document.actions = {
        {
            .action = {.world_id = "starter-b", .shard = "beta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 3},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kClaimed,
        },
        {
            .action = {.world_id = "starter-c", .shard = "gamma", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 4},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kFailed,
        },
        {
            .action = {.world_id = "starter-d", .shard = "delta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kCompleted,
        },
    };

    const auto status = server::core::worlds::evaluate_topology_actuation_realization_status(
        execution_document,
        request_document,
        desired,
        observed);
    EXPECT_TRUE(status.summary.request_present);
    EXPECT_TRUE(status.summary.execution_present);
    EXPECT_EQ(status.summary.actions_total, 5u);
    EXPECT_EQ(status.summary.available_actions, 1u);
    EXPECT_EQ(status.summary.claimed_actions, 1u);
    EXPECT_EQ(status.summary.failed_actions, 1u);
    EXPECT_EQ(status.summary.realized_actions, 1u);
    EXPECT_EQ(status.summary.awaiting_observation_actions, 0u);
    EXPECT_EQ(status.summary.stale_actions, 1u);

    const auto realized = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-d";
    });
    ASSERT_NE(realized, status.actions.end());
    EXPECT_EQ(realized->state, server::core::worlds::TopologyActuationRealizationState::kRealized);
    EXPECT_EQ(realized->observed_instances_before, 1u);
    EXPECT_EQ(realized->current_observed_instances, 2u);
}

TEST(MmorpgTopologyContractTest, EvaluatesActuationAdapterStatusFromExecutionAndRealization) {
    server::core::worlds::DesiredTopologyDocument desired;
    desired.topology_id = "starter";
    desired.revision = 11;
    desired.pools = {
        {.world_id = "starter-a", .shard = "alpha", .replicas = 3},
        {.world_id = "starter-b", .shard = "beta", .replicas = 4},
        {.world_id = "starter-c", .shard = "gamma", .replicas = 5},
        {.world_id = "starter-d", .shard = "delta", .replicas = 2},
        {.world_id = "starter-e", .shard = "epsilon", .replicas = 2},
        {.world_id = "starter-f", .shard = "zeta", .replicas = 1},
    };

    const std::vector<server::core::worlds::ObservedTopologyPool> observed{
        {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-b", .shard = "beta", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-c", .shard = "gamma", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-d", .shard = "delta", .instances = 2, .ready_instances = 1},
        {.world_id = "starter-e", .shard = "epsilon", .instances = 1, .ready_instances = 1},
        {.world_id = "starter-f", .shard = "zeta", .instances = 1, .ready_instances = 1},
    };

    server::core::worlds::TopologyActuationRequestDocument request_document;
    request_document.request_id = "adapter-boundary";
    request_document.revision = 6;
    request_document.basis_topology_revision = 11;
    request_document.actions = {
        {.world_id = "starter-a", .shard = "alpha", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
        {.world_id = "starter-b", .shard = "beta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 3},
        {.world_id = "starter-c", .shard = "gamma", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 4},
        {.world_id = "starter-d", .shard = "delta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
        {.world_id = "starter-e", .shard = "epsilon", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
        {.world_id = "starter-f", .shard = "zeta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
    };

    server::core::worlds::TopologyActuationExecutionDocument execution_document;
    execution_document.executor_id = "executor-a";
    execution_document.revision = 3;
    execution_document.request_revision = 6;
    execution_document.actions = {
        {
            .action = {.world_id = "starter-b", .shard = "beta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 3},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kClaimed,
        },
        {
            .action = {.world_id = "starter-c", .shard = "gamma", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 4},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kFailed,
        },
        {
            .action = {.world_id = "starter-d", .shard = "delta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kCompleted,
        },
        {
            .action = {.world_id = "starter-f", .shard = "zeta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
            .observed_instances_before = 1,
            .ready_instances_before = 1,
            .state = server::core::worlds::TopologyActuationExecutionActionState::kCompleted,
        },
    };

    server::core::worlds::TopologyActuationAdapterLeaseDocument lease_document;
    lease_document.adapter_id = "adapter-a";
    lease_document.execution_revision = 3;
    lease_document.actions = {
        {.world_id = "starter-b", .shard = "beta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 3},
        {.world_id = "starter-c", .shard = "gamma", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 4},
        {.world_id = "starter-d", .shard = "delta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
        {.world_id = "starter-e", .shard = "epsilon", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 1},
        {.world_id = "starter-f", .shard = "zeta", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool, .replica_delta = 2},
    };

    const auto status = server::core::worlds::evaluate_topology_actuation_adapter_status(
        lease_document,
        execution_document,
        request_document,
        desired,
        observed);
    EXPECT_TRUE(status.summary.execution_present);
    EXPECT_TRUE(status.summary.lease_present);
    EXPECT_TRUE(status.summary.lease_revision_matches_current_execution);
    EXPECT_EQ(status.summary.actions_total, 6u);
    EXPECT_EQ(status.summary.available_actions, 1u);
    EXPECT_EQ(status.summary.leased_actions, 1u);
    EXPECT_EQ(status.summary.failed_actions, 1u);
    EXPECT_EQ(status.summary.realized_actions, 1u);
    EXPECT_EQ(status.summary.awaiting_realization_actions, 1u);
    EXPECT_EQ(status.summary.stale_actions, 1u);

    const auto available = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-a";
    });
    ASSERT_NE(available, status.actions.end());
    EXPECT_EQ(available->state, server::core::worlds::TopologyActuationAdapterStatusState::kAvailable);

    const auto leased = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-b";
    });
    ASSERT_NE(leased, status.actions.end());
    EXPECT_EQ(leased->state, server::core::worlds::TopologyActuationAdapterStatusState::kLeased);

    const auto failed = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-c";
    });
    ASSERT_NE(failed, status.actions.end());
    EXPECT_EQ(failed->state, server::core::worlds::TopologyActuationAdapterStatusState::kFailed);

    const auto realized = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-d";
    });
    ASSERT_NE(realized, status.actions.end());
    EXPECT_EQ(realized->state, server::core::worlds::TopologyActuationAdapterStatusState::kRealized);

    const auto stale = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-e";
    });
    ASSERT_NE(stale, status.actions.end());
    EXPECT_EQ(stale->state, server::core::worlds::TopologyActuationAdapterStatusState::kStale);

    const auto awaiting = std::find_if(status.actions.begin(), status.actions.end(), [](const auto& action) {
        return action.world_id == "starter-f";
    });
    ASSERT_NE(awaiting, status.actions.end());
    EXPECT_EQ(awaiting->state, server::core::worlds::TopologyActuationAdapterStatusState::kAwaitingRealization);
}

TEST(MmorpgTopologyContractTest, FindsRuntimeAssignmentByInstanceId) {
    server::core::worlds::TopologyActuationRuntimeAssignmentDocument document;
    document.adapter_id = "adapter-a";
    document.revision = 4;
    document.lease_revision = 2;
    document.assignments = {
        {
            .instance_id = "server-1",
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        },
        {
            .instance_id = "server-2",
            .world_id = "starter-b",
            .shard = "beta",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        },
    };

    const auto* assignment = server::core::worlds::find_topology_actuation_runtime_assignment(
        document,
        "server-2");
    ASSERT_NE(assignment, nullptr);
    EXPECT_EQ(assignment->world_id, "starter-b");
    EXPECT_EQ(assignment->shard, "beta");
    EXPECT_EQ(assignment->action, server::core::worlds::TopologyActuationActionKind::kScaleOutPool);

    EXPECT_EQ(
        server::core::worlds::find_topology_actuation_runtime_assignment(document, "missing"),
        nullptr);
}
