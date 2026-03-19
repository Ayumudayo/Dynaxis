#include <gtest/gtest.h>

#include <server/core/worlds/kubernetes.hpp>

TEST(WorldsKubernetesContractTest, BuildsBindingFromDesiredPoolAndSanitizesNames) {
    server::core::worlds::DesiredTopologyPool pool;
    pool.world_id = "Starter A";
    pool.shard = "Alpha/1";
    pool.replicas = 3;
    pool.capacity_class = "burst";
    pool.placement_tags = {"ssd", "zone-a"};

    const auto binding = server::core::worlds::make_kubernetes_pool_binding(
        pool,
        "dynaxis-dev",
        server::core::worlds::KubernetesWorkloadKind::kStatefulSet);

    EXPECT_EQ(binding.world_id, "Starter A");
    EXPECT_EQ(binding.shard, "Alpha/1");
    EXPECT_EQ(binding.namespace_name, "dynaxis-dev");
    EXPECT_EQ(binding.workload_name, "world-set-starter-a-alpha-1");
    EXPECT_EQ(binding.target_replicas, 3u);
    EXPECT_EQ(binding.capacity_class, "burst");
    ASSERT_EQ(binding.placement_tags.size(), 2u);
    EXPECT_EQ(binding.placement_tags[0], "ssd");
}

TEST(WorldsKubernetesContractTest, CountsRuntimeAssignmentsForPoolAndAction) {
    server::core::worlds::TopologyActuationRuntimeAssignmentDocument document;
    document.assignments = {
        {.instance_id = "server-1", .world_id = "starter-a", .shard = "alpha", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool},
        {.instance_id = "server-2", .world_id = "starter-a", .shard = "alpha", .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool},
        {.instance_id = "server-3", .world_id = "starter-a", .shard = "alpha", .action = server::core::worlds::TopologyActuationActionKind::kScaleInPool},
    };

    EXPECT_EQ(
        server::core::worlds::count_topology_actuation_runtime_assignments(
            document,
            "starter-a",
            "alpha",
            server::core::worlds::TopologyActuationActionKind::kScaleOutPool),
        2u);
}

TEST(WorldsKubernetesContractTest, EvaluatesScaleOutBeforeReplicaPatchAsScaleWorkload) {
    const server::core::worlds::KubernetesPoolBinding binding{
        .world_id = "starter-a",
        .shard = "alpha",
        .namespace_name = "dynaxis-dev",
        .workload_name = "world-set-starter-a-alpha",
        .workload_kind = server::core::worlds::KubernetesWorkloadKind::kStatefulSet,
        .target_replicas = 3,
    };
    const server::core::worlds::KubernetesPoolObservation observation{
        .current_spec_replicas = 1,
        .ready_replicas = 1,
        .available_replicas = 1,
        .assigned_runtime_instances = 0,
        .idle_ready_runtime_instances = 1,
    };
    const server::core::worlds::TopologyActuationAdapterLeaseAction lease_action{
        .world_id = "starter-a",
        .shard = "alpha",
        .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        .replica_delta = 2,
    };

    const auto status = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        binding,
        observation,
        lease_action);

    EXPECT_EQ(status.phase, server::core::worlds::KubernetesPoolOrchestrationPhase::kScaleWorkload);
    EXPECT_EQ(status.next_action, server::core::worlds::KubernetesPoolNextAction::kPatchWorkloadReplicas);
    EXPECT_FALSE(status.summary.binding_target_reached);
}

TEST(WorldsKubernetesContractTest, EvaluatesScaleOutAfterReadyPodsAsAwaitRuntimeAssignment) {
    const server::core::worlds::KubernetesPoolBinding binding{
        .world_id = "starter-a",
        .shard = "alpha",
        .namespace_name = "dynaxis-dev",
        .workload_name = "world-set-starter-a-alpha",
        .workload_kind = server::core::worlds::KubernetesWorkloadKind::kStatefulSet,
        .target_replicas = 3,
    };
    const server::core::worlds::KubernetesPoolObservation observation{
        .current_spec_replicas = 3,
        .ready_replicas = 3,
        .available_replicas = 3,
        .assigned_runtime_instances = 1,
        .idle_ready_runtime_instances = 2,
    };
    const server::core::worlds::TopologyActuationAdapterLeaseAction lease_action{
        .world_id = "starter-a",
        .shard = "alpha",
        .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        .replica_delta = 2,
    };

    const auto status = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        binding,
        observation,
        lease_action);

    EXPECT_EQ(
        status.phase,
        server::core::worlds::KubernetesPoolOrchestrationPhase::kAwaitRuntimeAssignment);
    EXPECT_EQ(
        status.next_action,
        server::core::worlds::KubernetesPoolNextAction::kPublishRuntimeAssignments);
    EXPECT_FALSE(status.summary.runtime_assignment_satisfied);
}

TEST(WorldsKubernetesContractTest, EvaluatesScaleInUsingDrainOrchestrationSignals) {
    const server::core::worlds::KubernetesPoolBinding binding{
        .world_id = "starter-a",
        .shard = "alpha",
        .namespace_name = "dynaxis-dev",
        .workload_name = "world-set-starter-a-alpha",
        .workload_kind = server::core::worlds::KubernetesWorkloadKind::kStatefulSet,
        .target_replicas = 1,
    };
    const server::core::worlds::KubernetesPoolObservation observation{
        .current_spec_replicas = 3,
        .ready_replicas = 3,
        .available_replicas = 3,
    };
    const server::core::worlds::TopologyActuationAdapterLeaseAction lease_action{
        .world_id = "starter-a",
        .shard = "alpha",
        .action = server::core::worlds::TopologyActuationActionKind::kScaleInPool,
        .replica_delta = 2,
    };
    const server::core::worlds::WorldDrainOrchestrationStatus drain{
        .world_id = "starter-a",
        .drain_phase = server::core::worlds::WorldDrainPhase::kDrained,
        .phase = server::core::worlds::WorldDrainOrchestrationPhase::kAwaitingOwnerTransfer,
        .next_action = server::core::worlds::WorldDrainNextAction::kCommitOwnerTransfer,
        .target_owner_instance_id = "server-2",
    };

    const auto status = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        binding,
        observation,
        lease_action,
        drain);

    EXPECT_EQ(
        status.phase,
        server::core::worlds::KubernetesPoolOrchestrationPhase::kAwaitOwnerTransfer);
    EXPECT_EQ(
        status.next_action,
        server::core::worlds::KubernetesPoolNextAction::kCommitOwnerTransfer);
    EXPECT_EQ(status.target_owner_instance_id, "server-2");
}

TEST(WorldsKubernetesContractTest, EvaluatesScaleInReadyToClearAsRetireWorkloadThenComplete) {
    const server::core::worlds::KubernetesPoolBinding binding{
        .world_id = "starter-a",
        .shard = "alpha",
        .namespace_name = "dynaxis-dev",
        .workload_name = "world-set-starter-a-alpha",
        .workload_kind = server::core::worlds::KubernetesWorkloadKind::kStatefulSet,
        .target_replicas = 1,
    };
    const server::core::worlds::TopologyActuationAdapterLeaseAction lease_action{
        .world_id = "starter-a",
        .shard = "alpha",
        .action = server::core::worlds::TopologyActuationActionKind::kScaleInPool,
        .replica_delta = 2,
    };
    const server::core::worlds::WorldDrainOrchestrationStatus drain{
        .world_id = "starter-a",
        .drain_phase = server::core::worlds::WorldDrainPhase::kDrained,
        .phase = server::core::worlds::WorldDrainOrchestrationPhase::kReadyToClear,
        .next_action = server::core::worlds::WorldDrainNextAction::kClearPolicy,
    };

    const auto retire_status = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        binding,
        server::core::worlds::KubernetesPoolObservation{
            .current_spec_replicas = 3,
            .ready_replicas = 3,
            .available_replicas = 3,
        },
        lease_action,
        drain);

    EXPECT_EQ(
        retire_status.phase,
        server::core::worlds::KubernetesPoolOrchestrationPhase::kRetireWorkload);
    EXPECT_EQ(
        retire_status.next_action,
        server::core::worlds::KubernetesPoolNextAction::kPatchWorkloadRetirement);

    const auto complete_status = server::core::worlds::evaluate_kubernetes_pool_orchestration(
        binding,
        server::core::worlds::KubernetesPoolObservation{
            .current_spec_replicas = 1,
            .ready_replicas = 1,
            .available_replicas = 1,
        },
        lease_action,
        drain);

    EXPECT_EQ(
        complete_status.phase,
        server::core::worlds::KubernetesPoolOrchestrationPhase::kComplete);
    EXPECT_EQ(
        complete_status.next_action,
        server::core::worlds::KubernetesPoolNextAction::kNone);
}
