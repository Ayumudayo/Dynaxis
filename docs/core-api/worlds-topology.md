# World Topology API Guide

## Stability

| Header | Stability |
|---|---|
| `server/core/worlds/topology.hpp` | `[Stable]` |

## Canonical Naming

- canonical include path: `server/core/worlds/topology.hpp`
- canonical namespace: `server::core::worlds`
- `server/core/mmorpg/topology.hpp`는 compatibility wrapper로만 유지된다

## Scope

- This surface defines the desired-topology document, the observed-topology reconciliation contract, the read-only topology actuation-plan contract, the revisioned topology actuation request/status contract, the executor-facing execution progress/status contract, the observed-topology realization/adoption contract, the adapter-facing lease/status contract, and the runtime-assignment document/helper contract.
- It stays separate from startup stack manifests, world lifecycle policy writes, and concrete orchestrator adapters.
- It does not implement reconciliation, owner transfer choreography, migration payload handoff, or autoscaling.

## Public Contract

- `DesiredTopologyPool` defines one pool intent:
  - `world_id`
  - `shard`
  - `replicas`
  - `capacity_class`
  - `placement_tags[]`
- `DesiredTopologyDocument` defines the revisioned desired document:
  - `topology_id`
  - `revision`
  - `updated_at_ms`
  - `pools[]`
- `ObservedTopologyInstance` is the minimal runtime read-model input for topology aggregation:
  - `instance_id`
  - `role`
  - `world_id`
  - `shard`
  - `ready`
- `ObservedTopologyPool` is the aggregated runtime pool view:
  - `world_id`
  - `shard`
  - `instances`
  - `ready_instances`
- `collect_observed_pools()` aggregates observed instances into runtime pools.
- `TopologyPoolStatus` classifies desired-vs-observed outcomes:
  - `aligned`
  - `missing_observed_pool`
  - `under_replicated`
  - `over_replicated`
  - `no_ready_instances`
  - `undeclared_observed_pool`
- `reconcile_topology()` compares an optional desired document against observed pools and returns:
  - `TopologyReconciliationSummary`
  - `ReconciledTopologyPool[]`
- `TopologyActuationActionKind` converts non-aligned reconciliation states into explicit next-action categories:
  - `scale_out_pool`
  - `scale_in_pool`
  - `restore_pool_readiness`
  - `observe_undeclared_pool`
- `plan_topology_actuation()` returns:
  - `TopologyActuationPlanSummary`
  - `TopologyActuationAction[]`
- `TopologyActuationRequestDocument` defines a revisioned operator-approved actuation request:
  - `request_id`
  - `revision`
  - `requested_at_ms`
  - `basis_topology_revision`
  - `actions[]`
- `TopologyActuationRequestAction` records one requested pool action:
  - `world_id`
  - `shard`
  - `action`
  - `replica_delta`
- `TopologyActuationRequestActionState` classifies requested-action status:
  - `pending`
  - `satisfied`
  - `superseded`
- `evaluate_topology_actuation_request_status()` returns:
  - `TopologyActuationRequestStatusSummary`
  - `TopologyActuationRequestActionStatus[]`
- `TopologyActuationExecutionDocument` defines revisioned executor progress against a stored actuation request:
  - `executor_id`
  - `revision`
  - `updated_at_ms`
  - `request_revision`
  - `actions[]`
- `TopologyActuationExecutionItem` records one executor progress item:
  - `action`
  - `observed_instances_before`
  - `ready_instances_before`
  - `state` (`claimed|completed|failed`)
- `evaluate_topology_actuation_execution_status()` returns:
  - `TopologyActuationExecutionStatusSummary`
  - `TopologyActuationExecutionActionStatus[]`
- `evaluate_topology_actuation_realization_status()` returns:
  - `TopologyActuationRealizationStatusSummary`
  - `TopologyActuationRealizationActionStatus[]`
- `TopologyActuationAdapterLeaseDocument` defines revisioned adapter-facing lease state against a stored execution revision:
  - `adapter_id`
  - `revision`
  - `leased_at_ms`
  - `execution_revision`
  - `actions[]`
- `TopologyActuationAdapterLeaseAction` records one leased pool action:
  - `world_id`
  - `shard`
  - `action`
  - `replica_delta`
- `evaluate_topology_actuation_adapter_status()` returns:
  - `TopologyActuationAdapterStatusSummary`
  - `TopologyActuationAdapterStatusAction[]`
- `TopologyActuationRuntimeAssignmentDocument` defines revisioned runtime assignment state layered above a stored adapter lease:
  - `adapter_id`
  - `revision`
  - `updated_at_ms`
  - `lease_revision`
  - `assignments[]`
- `TopologyActuationRuntimeAssignmentItem` records one concrete instance retarget:
  - `instance_id`
  - `world_id`
  - `shard`
  - `action`
- `find_topology_actuation_runtime_assignment()` returns the instance-level assignment matching one `instance_id`, or `nullptr` when no assignment exists.

## Semantics

- only `role=="server"` instances contribute to observed pools
- pools are keyed by `world_id + shard`
- when no desired document exists, all observed pools are treated as `undeclared_observed_pool`
- `no_ready_instances` wins over replica-count comparison when a desired pool exists but every observed instance in that pool is unready
- actuation planning is read-only and derived from reconciliation:
  - `missing_observed_pool` / `under_replicated` -> `scale_out_pool`
  - `over_replicated` -> `scale_in_pool`
  - `no_ready_instances` -> `restore_pool_readiness`
  - `undeclared_observed_pool` -> `observe_undeclared_pool`
- actuation request documents stay separate from desired topology:
  - desired topology says what pool shape should exist
  - read-only actuation planning says what action would be needed now
  - actuation request says which actionable plan items an operator has approved
- requested-action status is derived from the current actuation plan:
  - `pending` means the current actionable plan still matches the stored request item
  - `satisfied` means the requested pool no longer has an actuation item in the current plan
  - `superseded` means the pool still has an actuation item, but the current action or replica delta no longer matches the stored request item
- `observe_undeclared_pool` stays read-only and should not be stored as an actuation request item
- execution progress stays separate from actuation request:
  - the request says which actionable pool items operators approved
  - the execution document says which of those approved items an executor has claimed or acknowledged
  - the derived execution status reports whether a requested item is currently `available`, `claimed`, `completed`, `failed`, or `stale`
- execution status is derived from both the stored request document and the current request-status view:
  - `available` means the request item is still pending and no executor progress item is stored for it
  - `claimed` means the request item is still pending and the executor has claimed it
  - `completed` means the executor reported completion and the request item is now satisfied
  - `failed` means the request item is still pending and the executor reported failure
  - `stale` means the executor progress no longer matches the current request/status surface
- realization/adoption stays separate from executor progress:
  - the execution document stores the observed pool baseline seen by the executor when it claimed the action
  - the derived realization status compares that baseline to current observed topology so self-reported `completed` and actual observed adoption are not conflated
- realization status is derived from the stored execution baseline plus current observed pools:
  - `available` means the action is still request-pending and no executor progress exists yet
  - `claimed` means the executor has claimed the action but not completed it yet
  - `awaiting_observation` means the executor reported completion, but current observed topology has not yet shown the expected delta from the stored baseline
  - `realized` means the executor reported completion and current observed topology shows the expected delta from the stored baseline
  - `failed` means the executor reported failure while the request item is still pending
  - `stale` means the execution progress no longer matches the current request/status surface
- adapter lease state stays separate from request, execution, and realization:
  - the lease document says which current execution items a concrete scaling adapter has claimed for enactment
  - the derived adapter status combines the stored lease with the current execution/realization surface so stale or superseded leases are visible immediately
- adapter status is derived from the current execution/realization surface plus an optional stored lease:
  - `available` means there is no current matching lease for the action
  - `leased` means a current matching lease exists and the action is still in an active claimed state
  - `awaiting_realization` means the adapter still holds the lease and the underlying execution item is waiting for observed-topology adoption
  - `realized` means the adapter-held action now has observed-topology adoption evidence
  - `failed` means the adapter-held action failed while the lease still matches the current execution surface
  - `stale` means the lease or underlying execution item no longer matches the current request/execution/realization surface
- runtime assignment stays separate from the adapter lease:
  - the adapter lease names which pool actions an adapter owns
  - the runtime assignment document names which concrete running instances that adapter has retargeted for those leased actions
  - one assignment item corresponds to one concrete instance retarget, not to an entire pool delta budget
- the stable helper intentionally stays narrow:
  - it lets a consumer locate the runtime assignment for one `instance_id`
  - it does not prescribe how a runtime, orchestrator adapter, or control-plane service serializes, applies, or persists that document

## Non-Goals

- no startup manifest parsing
- no Redis/document storage semantics
- no concrete orchestrator coupling
- no runtime process spawn/retire side effect
- no concrete Docker/Kubernetes/cloud adapter protocol for live scaling
- no concrete autoscaler or scheduler adapter
- no owner transfer choreography
- no migration/session-state handoff

## Public Proof

- unit contract: `MmorpgTopologyContractTest.*`
- public-api smoke: `core_public_api_smoke`
- installed consumer: `CoreInstalledPackageConsumer`

