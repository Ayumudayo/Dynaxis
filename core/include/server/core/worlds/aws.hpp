#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "server/core/worlds/kubernetes.hpp"
#include "server/core/worlds/topology.hpp"

namespace server::core::worlds {

enum class AwsLoadBalancerScheme : std::uint8_t {
    kInternal = 0,
    kInternetFacing,
};

inline constexpr std::string_view aws_load_balancer_scheme_name(AwsLoadBalancerScheme scheme) noexcept {
    switch (scheme) {
    case AwsLoadBalancerScheme::kInternal:
        return "internal";
    case AwsLoadBalancerScheme::kInternetFacing:
        return "internet-facing";
    }
    return "internal";
}

inline std::optional<AwsLoadBalancerScheme> parse_aws_load_balancer_scheme(
    std::string_view value) noexcept {
    if (value == "internal") {
        return AwsLoadBalancerScheme::kInternal;
    }
    if (value == "internet-facing") {
        return AwsLoadBalancerScheme::kInternetFacing;
    }
    return std::nullopt;
}

enum class AwsLoadBalancerType : std::uint8_t {
    kNetwork = 0,
    kApplication,
};

inline constexpr std::string_view aws_load_balancer_type_name(AwsLoadBalancerType type) noexcept {
    switch (type) {
    case AwsLoadBalancerType::kNetwork:
        return "network";
    case AwsLoadBalancerType::kApplication:
        return "application";
    }
    return "network";
}

inline std::optional<AwsLoadBalancerType> parse_aws_load_balancer_type(
    std::string_view value) noexcept {
    if (value == "network" || value == "nlb") {
        return AwsLoadBalancerType::kNetwork;
    }
    if (value == "application" || value == "alb") {
        return AwsLoadBalancerType::kApplication;
    }
    return std::nullopt;
}

enum class AwsLoadBalancerTargetKind : std::uint8_t {
    kIp = 0,
    kInstance,
};

inline constexpr std::string_view aws_load_balancer_target_kind_name(
    AwsLoadBalancerTargetKind kind) noexcept {
    switch (kind) {
    case AwsLoadBalancerTargetKind::kIp:
        return "ip";
    case AwsLoadBalancerTargetKind::kInstance:
        return "instance";
    }
    return "ip";
}

inline std::optional<AwsLoadBalancerTargetKind> parse_aws_load_balancer_target_kind(
    std::string_view value) noexcept {
    if (value == "ip") {
        return AwsLoadBalancerTargetKind::kIp;
    }
    if (value == "instance") {
        return AwsLoadBalancerTargetKind::kInstance;
    }
    return std::nullopt;
}

/** @brief one world pool이 배치될 AWS region / AZ / subnet target입니다. */
struct AwsPlacementTarget {
    std::string region;
    std::vector<std::string> availability_zones;
    std::vector<std::string> subnet_ids;
};

/** @brief one world pool의 AWS/EKS-side canonical identity naming입니다. */
struct AwsPoolIdentity {
    std::string cluster_name;
    std::string namespace_name;
    std::string workload_name;
    std::string service_name;
    std::string iam_role_name;
};

/** @brief one world pool이 요구하는 AWS load balancer attachment naming/shape입니다. */
struct AwsLoadBalancerAttachment {
    std::string kubernetes_service_name;
    std::string load_balancer_name;
    std::string target_group_name;
    AwsLoadBalancerType type{AwsLoadBalancerType::kNetwork};
    AwsLoadBalancerScheme scheme{AwsLoadBalancerScheme::kInternal};
    AwsLoadBalancerTargetKind target_kind{AwsLoadBalancerTargetKind::kIp};
    std::uint16_t listener_port{0};
    bool preserve_client_ip{true};
};

/** @brief one world pool이 기대하는 managed Redis/Postgres naming convention bundle입니다. */
struct AwsManagedDependencyConventions {
    std::string redis_replication_group_id;
    std::string redis_subnet_group_name;
    std::string postgres_cluster_id;
    std::string postgres_subnet_group_name;
    std::string postgres_secret_name;
};

/** @brief provider adapter가 generic pool metadata를 해석할 때 쓰는 AWS-side default policy입니다. */
struct AwsAdapterDefaults {
    std::string cluster_name;
    AwsPlacementTarget placement;
    AwsLoadBalancerType load_balancer_type{AwsLoadBalancerType::kNetwork};
    AwsLoadBalancerScheme load_balancer_scheme{AwsLoadBalancerScheme::kInternal};
    AwsLoadBalancerTargetKind load_balancer_target_kind{AwsLoadBalancerTargetKind::kIp};
    std::uint16_t listener_port{7000};
    bool preserve_client_ip{true};
    std::string redis_prefix{"dynaxis-redis"};
    std::string postgres_prefix{"dynaxis-pg"};
};

/** @brief one Kubernetes-first pool binding을 AWS-first identity/placement/dependency contract로 확장한 결과입니다. */
struct AwsPoolBinding {
    std::string world_id;
    std::string shard;
    std::string capacity_class;
    std::vector<std::string> placement_tags;
    AwsPoolIdentity identity;
    AwsPlacementTarget placement;
    AwsLoadBalancerAttachment load_balancer;
    AwsManagedDependencyConventions managed_dependencies;
};

/** @brief provider adapter가 현재 관측한 load balancer attachment health facts입니다. */
struct AwsLoadBalancerObservation {
    bool load_balancer_attached{false};
    bool target_group_attached{false};
    bool targets_healthy{false};
};

/** @brief provider adapter가 현재 관측한 managed Redis/Postgres readiness facts입니다. */
struct AwsManagedDependencyObservation {
    bool redis_ready{false};
    bool postgres_ready{false};
};

enum class AwsPoolAdapterPhase : std::uint8_t {
    kIdle = 0,
    kAwaitLoadBalancerAttachment,
    kAwaitTargetHealth,
    kAwaitManagedDependencies,
    kAwaitRuntimeAssignment,
    kComplete,
    kStale,
};

inline constexpr std::string_view aws_pool_adapter_phase_name(AwsPoolAdapterPhase phase) noexcept {
    switch (phase) {
    case AwsPoolAdapterPhase::kIdle:
        return "idle";
    case AwsPoolAdapterPhase::kAwaitLoadBalancerAttachment:
        return "await_load_balancer_attachment";
    case AwsPoolAdapterPhase::kAwaitTargetHealth:
        return "await_target_health";
    case AwsPoolAdapterPhase::kAwaitManagedDependencies:
        return "await_managed_dependencies";
    case AwsPoolAdapterPhase::kAwaitRuntimeAssignment:
        return "await_runtime_assignment";
    case AwsPoolAdapterPhase::kComplete:
        return "complete";
    case AwsPoolAdapterPhase::kStale:
        return "stale";
    }
    return "stale";
}

enum class AwsPoolAdapterNextAction : std::uint8_t {
    kNone = 0,
    kEnsureLoadBalancerAttachment,
    kWaitForTargetHealth,
    kEnsureManagedDependencies,
    kPublishRuntimeAssignments,
};

inline constexpr std::string_view aws_pool_adapter_next_action_name(
    AwsPoolAdapterNextAction action) noexcept {
    switch (action) {
    case AwsPoolAdapterNextAction::kNone:
        return "none";
    case AwsPoolAdapterNextAction::kEnsureLoadBalancerAttachment:
        return "ensure_load_balancer_attachment";
    case AwsPoolAdapterNextAction::kWaitForTargetHealth:
        return "wait_for_target_health";
    case AwsPoolAdapterNextAction::kEnsureManagedDependencies:
        return "ensure_managed_dependencies";
    case AwsPoolAdapterNextAction::kPublishRuntimeAssignments:
        return "publish_runtime_assignments";
    }
    return "none";
}

/** @brief AWS adapter status evaluation의 summarized readiness counters입니다. */
struct AwsPoolAdapterStatusSummary {
    bool action_present{false};
    bool load_balancer_ready{false};
    bool managed_dependencies_ready{false};
    bool runtime_assignment_satisfied{false};
    std::uint32_t assigned_runtime_instances{0};
    std::uint32_t required_runtime_assignments{0};
};

/** @brief one leased pool action을 AWS-first provider vocabulary로 평가한 결과입니다. */
struct AwsPoolAdapterStatus {
    std::string world_id;
    std::string shard;
    TopologyActuationActionKind requested_action{TopologyActuationActionKind::kObserveUndeclaredPool};
    AwsPoolAdapterPhase phase{AwsPoolAdapterPhase::kIdle};
    AwsPoolAdapterNextAction next_action{AwsPoolAdapterNextAction::kNone};
    AwsPoolAdapterStatusSummary summary;
};

inline std::optional<std::string> find_prefixed_world_tag_value(
    std::span<const std::string> tags,
    std::string_view prefix) {
    for (const auto& tag : tags) {
        if (std::string_view(tag).starts_with(prefix)) {
            return tag.substr(prefix.size());
        }
    }
    return std::nullopt;
}

inline std::vector<std::string> collect_prefixed_world_tag_values(
    std::span<const std::string> tags,
    std::string_view prefix) {
    std::vector<std::string> values;
    for (const auto& tag : tags) {
        if (std::string_view(tag).starts_with(prefix)) {
            values.emplace_back(tag.substr(prefix.size()));
        }
    }
    return values;
}

inline std::string make_aws_pool_service_name(std::string_view world_id, std::string_view shard) {
    return "world-svc-"
        + sanitize_kubernetes_name_token(world_id)
        + "-"
        + sanitize_kubernetes_name_token(shard);
}

inline std::string make_aws_pool_iam_role_name(std::string_view service_name) {
    return std::string(service_name) + "-role";
}

inline AwsPoolBinding make_aws_pool_binding(
    const KubernetesPoolBinding& binding,
    const AwsAdapterDefaults& defaults) {
    AwsPoolBinding out;
    out.world_id = binding.world_id;
    out.shard = binding.shard;
    out.capacity_class = binding.capacity_class;
    out.placement_tags = binding.placement_tags;
    out.identity.cluster_name = defaults.cluster_name;
    out.identity.namespace_name = binding.namespace_name;
    out.identity.workload_name = binding.workload_name;
    out.identity.service_name = make_aws_pool_service_name(binding.world_id, binding.shard);
    out.identity.iam_role_name = make_aws_pool_iam_role_name(out.identity.service_name);

    out.placement.region =
        find_prefixed_world_tag_value(binding.placement_tags, "region:").value_or(defaults.placement.region);

    auto availability_zones = collect_prefixed_world_tag_values(binding.placement_tags, "az:");
    if (availability_zones.empty()) {
        availability_zones = collect_prefixed_world_tag_values(binding.placement_tags, "zone:");
    }
    out.placement.availability_zones = availability_zones.empty()
        ? defaults.placement.availability_zones
        : std::move(availability_zones);

    auto subnet_ids = collect_prefixed_world_tag_values(binding.placement_tags, "subnet:");
    out.placement.subnet_ids = subnet_ids.empty()
        ? defaults.placement.subnet_ids
        : std::move(subnet_ids);

    const auto lb_scheme = find_prefixed_world_tag_value(binding.placement_tags, "aws-lb-scheme:");
    const auto lb_type = find_prefixed_world_tag_value(binding.placement_tags, "aws-lb-type:");
    const auto lb_target = find_prefixed_world_tag_value(binding.placement_tags, "aws-lb-target:");

    out.load_balancer.kubernetes_service_name = out.identity.service_name;
    out.load_balancer.type = lb_type.has_value()
        ? parse_aws_load_balancer_type(*lb_type).value_or(defaults.load_balancer_type)
        : defaults.load_balancer_type;
    out.load_balancer.scheme = lb_scheme.has_value()
        ? parse_aws_load_balancer_scheme(*lb_scheme).value_or(defaults.load_balancer_scheme)
        : defaults.load_balancer_scheme;
    out.load_balancer.target_kind = lb_target.has_value()
        ? parse_aws_load_balancer_target_kind(*lb_target).value_or(defaults.load_balancer_target_kind)
        : defaults.load_balancer_target_kind;
    out.load_balancer.listener_port = defaults.listener_port;
    out.load_balancer.preserve_client_ip = defaults.preserve_client_ip;
    out.load_balancer.load_balancer_name =
        std::string(out.load_balancer.type == AwsLoadBalancerType::kApplication ? "alb-" : "nlb-")
        + out.identity.service_name;
    out.load_balancer.target_group_name = "tg-" + out.identity.service_name;

    const auto world_token = sanitize_kubernetes_name_token(binding.world_id);
    const auto region_token = sanitize_kubernetes_name_token(out.placement.region.empty() ? "global" : out.placement.region);
    out.managed_dependencies.redis_replication_group_id =
        sanitize_kubernetes_name_token(defaults.redis_prefix) + "-" + world_token;
    out.managed_dependencies.redis_subnet_group_name =
        sanitize_kubernetes_name_token(defaults.redis_prefix) + "-" + region_token + "-subnets";
    out.managed_dependencies.postgres_cluster_id =
        sanitize_kubernetes_name_token(defaults.postgres_prefix) + "-" + world_token;
    out.managed_dependencies.postgres_subnet_group_name =
        sanitize_kubernetes_name_token(defaults.postgres_prefix) + "-" + region_token + "-subnets";
    out.managed_dependencies.postgres_secret_name =
        sanitize_kubernetes_name_token(defaults.postgres_prefix) + "-" + world_token + "-credentials";
    return out;
}

inline AwsPoolAdapterStatus evaluate_aws_pool_adapter_status(
    const AwsPoolBinding& binding,
    const AwsLoadBalancerObservation& load_balancer_observation,
    const AwsManagedDependencyObservation& dependency_observation,
    const std::optional<TopologyActuationAdapterLeaseAction>& lease_action = std::nullopt,
    const std::optional<TopologyActuationRuntimeAssignmentDocument>& runtime_assignment = std::nullopt) {
    AwsPoolAdapterStatus out;
    out.world_id = binding.world_id;
    out.shard = binding.shard;
    out.summary.action_present = lease_action.has_value();
    out.summary.load_balancer_ready =
        load_balancer_observation.load_balancer_attached
        && load_balancer_observation.target_group_attached
        && load_balancer_observation.targets_healthy;
    out.summary.managed_dependencies_ready =
        dependency_observation.redis_ready && dependency_observation.postgres_ready;

    if (!lease_action.has_value()) {
        out.phase = AwsPoolAdapterPhase::kIdle;
        out.next_action = AwsPoolAdapterNextAction::kNone;
        return out;
    }

    out.requested_action = lease_action->action;
    out.summary.assigned_runtime_instances = runtime_assignment.has_value()
        ? count_topology_actuation_runtime_assignments(
            *runtime_assignment,
            binding.world_id,
            binding.shard,
            lease_action->action)
        : 0;
    out.summary.required_runtime_assignments =
        lease_action->action == TopologyActuationActionKind::kScaleOutPool
        ? static_cast<std::uint32_t>(std::max(lease_action->replica_delta, 0))
        : 0;
    out.summary.runtime_assignment_satisfied =
        out.summary.assigned_runtime_instances >= out.summary.required_runtime_assignments;

    switch (lease_action->action) {
    case TopologyActuationActionKind::kObserveUndeclaredPool:
        out.phase = AwsPoolAdapterPhase::kIdle;
        out.next_action = AwsPoolAdapterNextAction::kNone;
        return out;

    case TopologyActuationActionKind::kScaleInPool:
        out.phase = AwsPoolAdapterPhase::kComplete;
        out.next_action = AwsPoolAdapterNextAction::kNone;
        return out;

    case TopologyActuationActionKind::kScaleOutPool:
    case TopologyActuationActionKind::kRestorePoolReadiness:
        if (!load_balancer_observation.load_balancer_attached) {
            out.phase = AwsPoolAdapterPhase::kAwaitLoadBalancerAttachment;
            out.next_action = AwsPoolAdapterNextAction::kEnsureLoadBalancerAttachment;
        } else if (!load_balancer_observation.target_group_attached
            || !load_balancer_observation.targets_healthy) {
            out.phase = AwsPoolAdapterPhase::kAwaitTargetHealth;
            out.next_action = AwsPoolAdapterNextAction::kWaitForTargetHealth;
        } else if (!out.summary.managed_dependencies_ready) {
            out.phase = AwsPoolAdapterPhase::kAwaitManagedDependencies;
            out.next_action = AwsPoolAdapterNextAction::kEnsureManagedDependencies;
        } else if (!out.summary.runtime_assignment_satisfied) {
            out.phase = AwsPoolAdapterPhase::kAwaitRuntimeAssignment;
            out.next_action = AwsPoolAdapterNextAction::kPublishRuntimeAssignments;
        } else {
            out.phase = AwsPoolAdapterPhase::kComplete;
            out.next_action = AwsPoolAdapterNextAction::kNone;
        }
        return out;
    }

    out.phase = AwsPoolAdapterPhase::kStale;
    out.next_action = AwsPoolAdapterNextAction::kNone;
    return out;
}

} // namespace server::core::worlds
