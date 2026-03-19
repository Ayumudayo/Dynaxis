# World Kubernetes API Guide

## Stability

| Header | Stability |
|---|---|
| `server/core/worlds/kubernetes.hpp` | `[Stable]` |

## Canonical Naming

- canonical include path: `server/core/worlds/kubernetes.hpp`
- canonical namespace: `server::core::worlds`

## Scope

- This surface connects the existing desired-topology / actuation / runtime-assignment / drain-orchestration layers to Kubernetes-first workload lifecycle vocabulary.
- It is intentionally read-only and contract-first:
  - workload binding
  - runtime observation counters
  - replica patch / readiness wait / runtime assignment / drain / owner-transfer / migration / retirement phase evaluation
- It does not embed `kubectl`, watch loops, CRDs, Helm charts, cloud-provider APIs, or manifest persistence.

## Public Contract

- `KubernetesWorkloadKind`
  - `deployment`
  - `statefulset`
- `KubernetesPoolBinding`
  - `world_id`
  - `shard`
  - `namespace_name`
  - `workload_name`
  - `workload_kind`
  - `target_replicas`
  - `capacity_class`
  - `placement_tags[]`
- `make_kubernetes_pool_binding()`
  - derives a canonical namespace/workload binding from one `DesiredTopologyPool`
- `make_kubernetes_pool_workload_name()`
  - derives a sanitized workload name from `world_id + shard`
- `KubernetesPoolObservation`
  - `current_spec_replicas`
  - `ready_replicas`
  - `available_replicas`
  - `terminating_replicas`
  - `assigned_runtime_instances`
  - `idle_ready_runtime_instances`
- `count_topology_actuation_runtime_assignments()`
  - counts runtime assignments for one `world_id + shard + action`
- `KubernetesPoolOrchestrationPhase`
  - `idle`
  - `scale_workload`
  - `await_ready_replicas`
  - `await_runtime_assignment`
  - `await_replacement_target`
  - `await_drain`
  - `await_owner_transfer`
  - `await_migration`
  - `retire_workload`
  - `complete`
  - `stale`
- `KubernetesPoolNextAction`
  - `none`
  - `patch_workload_replicas`
  - `wait_for_pods_ready`
  - `publish_runtime_assignments`
  - `stabilize_replacement_target`
  - `wait_for_drain`
  - `commit_owner_transfer`
  - `wait_for_migration`
  - `patch_workload_retirement`
- `evaluate_kubernetes_pool_orchestration()`
  - combines one binding, current workload/runtime observation, one optional adapter lease action, and one optional `WorldDrainOrchestrationStatus`
  - returns the current Kubernetes-first orchestration phase and next action for that pool

## Semantics

- `scale_out_pool`
  - below target replicas -> `scale_workload`
  - target replicas reached but pods not ready -> `await_ready_replicas`
  - pods ready but runtime assignment incomplete -> `await_runtime_assignment`
  - workload and assignment aligned -> `complete`
- `restore_pool_readiness`
  - below target replicas -> `scale_workload`
  - target replicas reached but zero ready pods -> `await_ready_replicas`
  - any ready pods restored -> `complete`
- `scale_in_pool`
  - `WorldDrainOrchestrationStatus` drives replacement/drain/transfer/migration phases first
  - once drain orchestration reaches `ready_to_clear`, the contract falls through to `retire_workload`
  - when current workload replicas already match the target and no retirement is in flight -> `complete`
- `observe_undeclared_pool`
  - remains non-mutating and maps to `idle`

## Non-Goals

- no manifest rendering format
- no Kubernetes API client
- no CRD/controller runtime
- no pod scheduling or service/LB attachment semantics
- no provider-specific metadata

Provider-specific LB / region / managed dependency mapping moved to:
- `docs/core-api/worlds-aws.md`

## Public Proof

- unit contract: `WorldsKubernetesContractTest.*`
- public-api smoke: `core_public_api_smoke`
- stable-header scenarios: `core_public_api_stable_header_scenarios`
- installed consumer: `CoreInstalledPackageConsumer`
