# World AWS API Guide

## Stability

| Header | Stability |
|---|---|
| `server/core/worlds/aws.hpp` | `[Stable]` |

## Canonical Naming

- canonical include path: `server/core/worlds/aws.hpp`
- canonical namespace: `server::core::worlds`

## Scope

- This surface sits above the current generic topology/adapter lease/runtime-assignment model and the provider-neutral Kubernetes pool binding.
- It interprets one world pool as an AWS-first provider adapter binding for:
  - workload identity on EKS-like clusters
  - load balancer attachment and target-group naming
  - managed Redis/Postgres naming conventions
  - region / availability-zone / subnet placement metadata
- It intentionally stays contract-first and read-only:
  - no AWS SDK client
  - no CloudFormation/Terraform payload
  - no EKS or ELB mutation loop
  - no live IAM, RDS, or ElastiCache orchestration side effect

## Public Contract

- `AwsLoadBalancerScheme`
  - `internal`
  - `internet-facing`
- `AwsLoadBalancerType`
  - `network`
  - `application`
- `AwsLoadBalancerTargetKind`
  - `ip`
  - `instance`
- `AwsPlacementTarget`
  - `region`
  - `availability_zones[]`
  - `subnet_ids[]`
- `AwsPoolIdentity`
  - `cluster_name`
  - `namespace_name`
  - `workload_name`
  - `service_name`
  - `iam_role_name`
- `AwsLoadBalancerAttachment`
  - `kubernetes_service_name`
  - `load_balancer_name`
  - `target_group_name`
  - `type`
  - `scheme`
  - `target_kind`
  - `listener_port`
  - `preserve_client_ip`
- `AwsManagedDependencyConventions`
  - `redis_replication_group_id`
  - `redis_subnet_group_name`
  - `postgres_cluster_id`
  - `postgres_subnet_group_name`
  - `postgres_secret_name`
- `AwsAdapterDefaults`
  - provider-wide defaults for cluster identity, placement, LB policy, and managed dependency naming prefixes
- `AwsPoolBinding`
  - one provider-specific binding derived from one `KubernetesPoolBinding`
- `AwsLoadBalancerObservation`
  - `load_balancer_attached`
  - `target_group_attached`
  - `targets_healthy`
- `AwsManagedDependencyObservation`
  - `redis_ready`
  - `postgres_ready`
- `AwsPoolAdapterPhase`
  - `idle`
  - `await_load_balancer_attachment`
  - `await_target_health`
  - `await_managed_dependencies`
  - `await_runtime_assignment`
  - `complete`
  - `stale`
- `AwsPoolAdapterNextAction`
  - `none`
  - `ensure_load_balancer_attachment`
  - `wait_for_target_health`
  - `ensure_managed_dependencies`
  - `publish_runtime_assignments`
- `make_aws_pool_binding()`
  - derives AWS-first cluster identity, service/LB names, managed dependency naming, and placement metadata from a provider-neutral `KubernetesPoolBinding`
- `evaluate_aws_pool_adapter_status()`
  - combines one binding, optional adapter lease, optional runtime assignment, load balancer observation, and managed dependency observation into a provider-facing status summary

## Tag Conventions

- Existing generic `placement_tags[]` remain the input seam.
- `make_aws_pool_binding()` understands these provider-oriented tag prefixes when present:
  - `region:<value>`
  - `az:<value>` or `zone:<value>`
  - `subnet:<value>`
  - `aws-lb-scheme:internal|internet-facing`
  - `aws-lb-type:nlb|alb`
  - `aws-lb-target:ip|instance`
- If a tag is missing, the function falls back to `AwsAdapterDefaults`.

## Semantics

- `scale_out_pool` / `restore_pool_readiness`
  - missing LB attachment -> `await_load_balancer_attachment`
  - LB attached but target group not ready -> `await_target_health`
  - managed Redis/Postgres convention not ready -> `await_managed_dependencies`
  - provider side ready but runtime assignment incomplete -> `await_runtime_assignment`
  - all provider/runtime preconditions satisfied -> `complete`
- `scale_in_pool`
  - current provider slice treats the provider mapping as already satisfied and returns `complete`
  - teardown choreography remains owned by the generic topology/drain/orchestration surfaces for now
- `observe_undeclared_pool`
  - remains non-mutating and maps to `idle`

## Non-Goals

- no direct AWS API authentication
- no ALB/NLB provisioning request schema
- no managed RDS/ElastiCache failover policy
- no cross-region data replication contract
- no concrete EKS manifest mutation loop

## Public Proof

- unit contract: `WorldsAwsContractTest.*`
- public-api smoke: `core_public_api_smoke`
- stable-header scenarios: `core_public_api_stable_header_scenarios`
- installed consumer: `CoreInstalledPackageConsumer`
