#include <gtest/gtest.h>

#include <server/core/worlds/aws.hpp>

TEST(WorldsAwsContractTest, BuildsAwsBindingFromKubernetesBindingAndPlacementTags) {
    const server::core::worlds::KubernetesPoolBinding binding{
        .world_id = "Starter A",
        .shard = "Alpha/1",
        .namespace_name = "dynaxis-prod",
        .workload_name = "world-set-starter-a-alpha-1",
        .workload_kind = server::core::worlds::KubernetesWorkloadKind::kStatefulSet,
        .target_replicas = 3,
        .capacity_class = "burst",
        .placement_tags = {
            "region:us-west-2",
            "az:us-west-2a",
            "az:us-west-2c",
            "subnet:subnet-a",
            "subnet:subnet-c",
            "aws-lb-scheme:internet-facing",
            "aws-lb-type:alb",
        },
    };
    const server::core::worlds::AwsAdapterDefaults defaults{
        .cluster_name = "eks-live",
        .placement = {
            .region = "ap-northeast-2",
            .availability_zones = {"ap-northeast-2a"},
            .subnet_ids = {"subnet-default"},
        },
        .listener_port = 7000,
        .redis_prefix = "chat-redis",
        .postgres_prefix = "chat-pg",
    };

    const auto aws_binding = server::core::worlds::make_aws_pool_binding(binding, defaults);

    EXPECT_EQ(aws_binding.identity.cluster_name, "eks-live");
    EXPECT_EQ(aws_binding.identity.namespace_name, "dynaxis-prod");
    EXPECT_EQ(aws_binding.identity.workload_name, "world-set-starter-a-alpha-1");
    EXPECT_EQ(aws_binding.identity.service_name, "world-svc-starter-a-alpha-1");
    EXPECT_EQ(aws_binding.identity.iam_role_name, "world-svc-starter-a-alpha-1-role");
    EXPECT_EQ(aws_binding.placement.region, "us-west-2");
    ASSERT_EQ(aws_binding.placement.availability_zones.size(), 2u);
    EXPECT_EQ(aws_binding.placement.availability_zones[0], "us-west-2a");
    EXPECT_EQ(aws_binding.placement.subnet_ids[1], "subnet-c");
    EXPECT_EQ(aws_binding.load_balancer.type, server::core::worlds::AwsLoadBalancerType::kApplication);
    EXPECT_EQ(
        aws_binding.load_balancer.scheme,
        server::core::worlds::AwsLoadBalancerScheme::kInternetFacing);
    EXPECT_EQ(aws_binding.load_balancer.load_balancer_name, "alb-world-svc-starter-a-alpha-1");
    EXPECT_EQ(aws_binding.managed_dependencies.redis_replication_group_id, "chat-redis-starter-a");
    EXPECT_EQ(aws_binding.managed_dependencies.postgres_cluster_id, "chat-pg-starter-a");
}

TEST(WorldsAwsContractTest, EvaluatesScaleOutAsAwaitLoadBalancerAttachmentBeforeProviderReady) {
    const auto status = server::core::worlds::evaluate_aws_pool_adapter_status(
        server::core::worlds::AwsPoolBinding{
            .world_id = "starter-a",
            .shard = "alpha",
        },
        server::core::worlds::AwsLoadBalancerObservation{
            .load_balancer_attached = false,
            .target_group_attached = false,
            .targets_healthy = false,
        },
        server::core::worlds::AwsManagedDependencyObservation{
            .redis_ready = true,
            .postgres_ready = true,
        },
        server::core::worlds::TopologyActuationAdapterLeaseAction{
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 2,
        });

    EXPECT_EQ(status.phase, server::core::worlds::AwsPoolAdapterPhase::kAwaitLoadBalancerAttachment);
    EXPECT_EQ(
        status.next_action,
        server::core::worlds::AwsPoolAdapterNextAction::kEnsureLoadBalancerAttachment);
}

TEST(WorldsAwsContractTest, EvaluatesScaleOutAsAwaitManagedDependenciesAfterTargetHealth) {
    const auto status = server::core::worlds::evaluate_aws_pool_adapter_status(
        server::core::worlds::AwsPoolBinding{
            .world_id = "starter-a",
            .shard = "alpha",
        },
        server::core::worlds::AwsLoadBalancerObservation{
            .load_balancer_attached = true,
            .target_group_attached = true,
            .targets_healthy = true,
        },
        server::core::worlds::AwsManagedDependencyObservation{
            .redis_ready = false,
            .postgres_ready = true,
        },
        server::core::worlds::TopologyActuationAdapterLeaseAction{
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 1,
        });

    EXPECT_EQ(status.phase, server::core::worlds::AwsPoolAdapterPhase::kAwaitManagedDependencies);
    EXPECT_EQ(
        status.next_action,
        server::core::worlds::AwsPoolAdapterNextAction::kEnsureManagedDependencies);
}

TEST(WorldsAwsContractTest, EvaluatesScaleOutAsCompleteWhenProviderAndAssignmentsAreReady) {
    server::core::worlds::TopologyActuationRuntimeAssignmentDocument assignments;
    assignments.assignments = {
        {
            .instance_id = "server-1",
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        },
        {
            .instance_id = "server-2",
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
        },
    };

    const auto status = server::core::worlds::evaluate_aws_pool_adapter_status(
        server::core::worlds::AwsPoolBinding{
            .world_id = "starter-a",
            .shard = "alpha",
        },
        server::core::worlds::AwsLoadBalancerObservation{
            .load_balancer_attached = true,
            .target_group_attached = true,
            .targets_healthy = true,
        },
        server::core::worlds::AwsManagedDependencyObservation{
            .redis_ready = true,
            .postgres_ready = true,
        },
        server::core::worlds::TopologyActuationAdapterLeaseAction{
            .world_id = "starter-a",
            .shard = "alpha",
            .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            .replica_delta = 2,
        },
        assignments);

    EXPECT_EQ(status.phase, server::core::worlds::AwsPoolAdapterPhase::kComplete);
    EXPECT_EQ(status.next_action, server::core::worlds::AwsPoolAdapterNextAction::kNone);
    EXPECT_TRUE(status.summary.runtime_assignment_satisfied);
    EXPECT_EQ(status.summary.assigned_runtime_instances, 2u);
}
