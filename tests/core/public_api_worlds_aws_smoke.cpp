#include <iostream>
#include <string>
#include <vector>

#include "server/core/api/version.hpp"
#include "server/core/worlds/aws.hpp"
#include "server/core/worlds/kubernetes.hpp"
#include "server/core/worlds/topology.hpp"

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
    (void)server::core::api::version_string();

    const server::core::worlds::DesiredTopologyPool desired_pool{
        .world_id = "starter-a",
        .shard = "alpha",
        .replicas = 2,
        .capacity_class = "burst",
        .placement_tags = {
            "region:us-west-2",
            "az:us-west-2a",
            "az:us-west-2c",
            "subnet:subnet-a",
            "subnet:subnet-c",
            "aws-lb-scheme:internet-facing",
            "aws-lb-type:nlb",
            "aws-lb-target:ip",
        },
    };

    const auto kubernetes_binding =
        server::core::worlds::make_kubernetes_pool_binding(desired_pool, "dynaxis-prod");
    if (!require_true(kubernetes_binding.workload_name == "world-set-starter-a-alpha", "kubernetes binding workload mismatch")) {
        return 1;
    }

    server::core::worlds::TopologyActuationRuntimeAssignmentDocument runtime_assignment{
        .adapter_id = "aws-adapter-a",
        .revision = 3,
        .lease_revision = 2,
        .assignments = {
            {
                .instance_id = "starter-a-alpha-0",
                .world_id = "starter-a",
                .shard = "alpha",
                .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            },
            {
                .instance_id = "starter-a-alpha-1",
                .world_id = "starter-a",
                .shard = "alpha",
                .action = server::core::worlds::TopologyActuationActionKind::kScaleOutPool,
            },
        },
    };
    if (!require_true(
            server::core::worlds::count_topology_actuation_runtime_assignments(
                runtime_assignment,
                "starter-a",
                "alpha",
                server::core::worlds::TopologyActuationActionKind::kScaleOutPool) == 2,
            "runtime assignment count should reflect both scale-out targets")) {
        return 1;
    }

    const auto aws_binding = server::core::worlds::make_aws_pool_binding(
        kubernetes_binding,
        server::core::worlds::AwsAdapterDefaults{
            .cluster_name = "eks-live",
            .placement = {
                .region = "ap-northeast-2",
                .availability_zones = {"ap-northeast-2a", "ap-northeast-2c"},
                .subnet_ids = {"subnet-default-a", "subnet-default-c"},
            },
            .load_balancer_type = server::core::worlds::AwsLoadBalancerType::kNetwork,
            .load_balancer_scheme = server::core::worlds::AwsLoadBalancerScheme::kInternal,
            .load_balancer_target_kind = server::core::worlds::AwsLoadBalancerTargetKind::kIp,
            .listener_port = 7000,
            .redis_prefix = "chat-redis",
            .postgres_prefix = "chat-pg",
        });

    if (!require_true(aws_binding.identity.cluster_name == "eks-live", "aws binding cluster mismatch")) {
        return 1;
    }
    if (!require_true(aws_binding.identity.namespace_name == "dynaxis-prod", "aws binding namespace mismatch")) {
        return 1;
    }
    if (!require_true(aws_binding.identity.service_name == "world-svc-starter-a-alpha", "aws binding service mismatch")) {
        return 1;
    }
    if (!require_true(aws_binding.placement.region == "us-west-2", "aws binding region should prefer placement tag")) {
        return 1;
    }
    if (!require_true(
            aws_binding.load_balancer.scheme == server::core::worlds::AwsLoadBalancerScheme::kInternetFacing,
            "aws binding should respect tagged LB scheme")) {
        return 1;
    }
    if (!require_true(
            aws_binding.managed_dependencies.redis_replication_group_id == "chat-redis-starter-a",
            "aws managed redis naming mismatch")) {
        return 1;
    }

    const auto provider_ready = server::core::worlds::evaluate_aws_pool_adapter_status(
        aws_binding,
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
        runtime_assignment);
    if (!require_true(
            provider_ready.phase == server::core::worlds::AwsPoolAdapterPhase::kComplete,
            "aws provider path should report complete when LB, dependencies, and assignments are ready")) {
        return 1;
    }

    const auto provider_waiting = server::core::worlds::evaluate_aws_pool_adapter_status(
        aws_binding,
        server::core::worlds::AwsLoadBalancerObservation{
            .load_balancer_attached = true,
            .target_group_attached = true,
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
        },
        runtime_assignment);
    if (!require_true(
            provider_waiting.phase == server::core::worlds::AwsPoolAdapterPhase::kAwaitTargetHealth,
            "aws provider path should block on target health before completion")) {
        return 1;
    }

    return 0;
}
