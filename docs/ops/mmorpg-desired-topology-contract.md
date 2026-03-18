# MMORPG Desired Topology Contract

This document defines the desired-topology contract for the MMORPG runtime.
The control-plane document API, reconciliation read model, and one narrow live runtime-assignment realization path now exist.

## Purpose

- keep current startup-only topology manifests from becoming the long-term scaling contract
- separate desired topology from observed topology and from world lifecycle policy
- define the future orchestration tranche without binding the design to a Docker-only or Kubernetes-only control plane

## Current Boundary

- startup topology control is still supported through:
  - `docker/stack/topologies/*.json`
  - `scripts/deploy_docker.ps1 -TopologyConfig <path>`
- live desired-topology document control now exists through:
  - `GET /api/v1/topology/desired`
  - `PUT /api/v1/topology/desired`
  - `DELETE /api/v1/topology/desired`
- live observed-topology read model now exists through:
  - `GET /api/v1/topology/observed`
- live desired-vs-observed reconciliation read model now exists through:
  - `GET /api/v1/topology/reconciliation`
- live read-only topology actuation plan now exists through:
  - `GET /api/v1/topology/actuation`
- revisioned topology actuation request/status control now exists through:
  - `GET /api/v1/topology/actuation/request`
  - `PUT /api/v1/topology/actuation/request`
  - `DELETE /api/v1/topology/actuation/request`
  - `GET /api/v1/topology/actuation/status`
- executor-facing actuation progress/status control now exists through:
  - `GET /api/v1/topology/actuation/execution`
  - `PUT /api/v1/topology/actuation/execution`
  - `DELETE /api/v1/topology/actuation/execution`
  - `GET /api/v1/topology/actuation/execution/status`
  - `GET /api/v1/topology/actuation/realization`
- adapter-facing actuation lease/status control now exists through:
  - `GET /api/v1/topology/actuation/adapter`
  - `PUT /api/v1/topology/actuation/adapter`
  - `DELETE /api/v1/topology/actuation/adapter`
  - `GET /api/v1/topology/actuation/adapter/status`
- runtime-assignment actuation control now exists through:
  - `GET /api/v1/topology/actuation/runtime-assignment`
  - `PUT /api/v1/topology/actuation/runtime-assignment`
  - `DELETE /api/v1/topology/actuation/runtime-assignment`
- current manifests are concrete local/proof artifacts
- world lifecycle policy remains a separate operator/runtime routing contract
- desired topology is currently stored as one revisioned document in the admin control plane; reconciliation status is visible, but no runtime reconciler or live pool actuation mutates instances from that document yet
- the new actuation plan narrows that blocker by making the required next actions explicit, the actuation request/status surface records operator-approved actions separately, the execution progress/status surface records executor-facing claim/ack progress separately, the realization surface now checks those claims against observed topology, the adapter surface records adapter-facing leases separately, and the runtime-assignment surface can now retarget idle running servers into leased scale-out pools, but it still does not spawn/retire processes or implement elastic autoscaling

## Canonical Desired Model

Desired topology is pool-based, not concrete-instance-based.

Desired state declares:

- which `world_id` / `shard` pools should exist
- how many replicas each pool should have
- optional placement/capacity hints

Desired state does not declare:

- concrete `instance_id`
- concrete container/pod names
- concrete host ports

## Minimal Shape

Top-level fields:

- `topology_id`
- `revision`
- `pools[]`

Each pool contains:

- `world_id`
- `shard`
- `replicas`
- `capacity_class` optional
- `placement_tags[]` optional

## Desired vs Observed Topology

- desired topology answers "what pool shape do we want?"
- observed topology answers "which instances actually exist and which owner/read-model state is live?"
- observed topology is the only place where concrete instance placement, health, readiness, and owner visibility appear

## Relationship To World Lifecycle Policy

- desired topology must not absorb:
  - `draining`
  - `replacement_owner_instance_id`
  - owner continuity keys
- future implementations may coordinate desired topology with lifecycle policy, but the contracts remain separate

## Future Orchestration Boundary

- a future tranche may add:
  - pool-oriented scaling APIs
  - runtime reconciliation / actuation
- that tranche must stay orchestrator-agnostic at the contract level
- it must preserve the split between:
  - startup manifests for local/proof stacks
  - desired topology for capacity/placement intent
  - lifecycle policy for operator-visible drain/replacement control

## Explicit Non-Goals

- no concrete `instance_id` in desired topology
- no host port declarations
- no Docker-only or Kubernetes-only semantics in the contract itself
- no requirement that desired topology be stored in Redis
- no live scaling API shape committed in this document

## Current Admin API Shape

`desired topology` document:

- `topology_id`
- `revision`
- `updated_at_ms`
- `pools[]`

Each pool contains:

- `world_id`
- `shard`
- `replicas`
- `capacity_class` optional
- `placement_tags[]` optional

Write semantics:

- `PUT /api/v1/topology/desired` requires JSON body with `topology_id` + `pools[]`
- optional `expected_revision` applies optimistic revision checking
- successful writes allocate `revision = current_revision + 1`
- `DELETE /api/v1/topology/desired` clears the stored document

Observed topology read model:

- `GET /api/v1/topology/observed`
- returns current `instances[]`, `worlds[]`, `summary`, and `updated_at_ms`
- it is intentionally separate from the desired document and from world lifecycle policy writes

Reconciliation read model:

- `GET /api/v1/topology/reconciliation`
- returns the stored desired document, aggregated observed pools, and desired-vs-observed alignment summary
- it is still read-only evidence; it does not trigger runtime mutation or pool scaling

Actuation read model:

- `GET /api/v1/topology/actuation`
- returns the stored desired document, observed pools, and a read-only list of next actions such as `scale_out_pool`, `scale_in_pool`, `restore_pool_readiness`, or `observe_undeclared_pool`
- it narrows the live-topology blocker by turning mismatch into explicit action vocabulary, but it still does not mutate runtime topology

Actuation request document:

- `GET /api/v1/topology/actuation/request`
- `PUT /api/v1/topology/actuation/request`
- `DELETE /api/v1/topology/actuation/request`
- stores a separate revisioned operator-approved request document:
  - `request_id`
  - `revision`
  - `requested_at_ms`
  - `basis_topology_revision`
  - `actions[]` with `world_id`, `shard`, `action`, `replica_delta`
- `PUT` only accepts currently actionable items from `GET /api/v1/topology/actuation`
- `observe_undeclared_pool` is not writable because it is an observe-only category

Actuation status read model:

- `GET /api/v1/topology/actuation/status`
- returns:
  - current desired topology
  - current stored actuation request document
  - observed pools
  - current read-only actuation plan summary
  - per-request action status classified as `pending`, `satisfied`, or `superseded`
- this still does not execute scaling; it only defines the narrowest request/status contract that a future runtime reconciler or external orchestrator adapter could consume

Execution progress document:

- `GET /api/v1/topology/actuation/execution`
- `PUT /api/v1/topology/actuation/execution`
- `DELETE /api/v1/topology/actuation/execution`
- stores a separate revisioned executor-facing progress document:
  - `executor_id`
  - `revision`
  - `updated_at_ms`
  - `request_revision`
  - `actions[]` with `world_id`, `shard`, `action`, `replica_delta`, `state`
- `PUT` only accepts items that match the current stored actuation request revision
- the surface is still orchestrator-agnostic; it names progress state, not container/pod/cloud API details
- `observed_instances_before` / `ready_instances_before` capture the executor's baseline observation so later realization can compare current observed topology to the original claim context

Execution status read model:

- `GET /api/v1/topology/actuation/execution/status`
- returns:
  - current stored actuation request document
  - current stored execution progress document
  - current request-summary
  - per-request execution status classified as `available`, `claimed`, `completed`, `failed`, or `stale`
- this defines the narrowest executor-facing claim/ack/status contract, but still does not perform live scaling

Realization read model:

- `GET /api/v1/topology/actuation/realization`
- returns:
  - current stored actuation request document
  - current stored execution progress document
  - current observed pools
  - current request-summary
  - current execution-summary
  - per-request realization status classified as `available`, `claimed`, `awaiting_observation`, `realized`, `failed`, or `stale`
- this is the first surface that explicitly distinguishes self-reported executor completion from actual observed-topology adoption

Adapter lease document:

- `GET /api/v1/topology/actuation/adapter`
- `PUT /api/v1/topology/actuation/adapter`
- `DELETE /api/v1/topology/actuation/adapter`
- stores a separate revisioned adapter-facing lease document:
  - `adapter_id`
  - `revision`
  - `leased_at_ms`
  - `execution_revision`
  - `actions[]` with `world_id`, `shard`, `action`, `replica_delta`
- `PUT` only accepts items that match the current execution revision and current non-stale execution/realization surface
- the surface is still orchestrator-agnostic; it records lease/claim intent, not concrete cloud or container API payloads

Adapter status read model:

- `GET /api/v1/topology/actuation/adapter/status`
- returns:
  - current stored actuation adapter lease document
  - current stored execution progress document
  - current execution-summary
  - current realization-summary
  - per-action adapter status classified as `available`, `leased`, `awaiting_realization`, `realized`, `failed`, or `stale`
- this defines the narrowest adapter-facing lease/status boundary that can sit above execution plus realization evidence without yet enacting runtime scale changes

Runtime assignment control:

- `GET /api/v1/topology/actuation/runtime-assignment`
- `PUT /api/v1/topology/actuation/runtime-assignment`
- `DELETE /api/v1/topology/actuation/runtime-assignment`
- stores a revisioned runtime-assignment document:
  - `adapter_id`
  - `revision`
  - `updated_at_ms`
  - `lease_revision`
  - `assignments[]` with `instance_id`, `world_id`, `shard`, `action`
- current implementation intentionally stays narrow:
  - it only accepts `scale_out_pool`
  - it only retargets `ready && active_sessions==0` running `server` instances
  - it consumes the current adapter lease rather than bypassing the existing request/execution/realization layers
- when the assignment document is present, `server_app` uses it to override the registry-reported `world:<id>` tag and shard for the matching `SERVER_INSTANCE_ID`, and it also uses the same override for fresh world admission defaults
- this is the first supported runtime path that can change observed topology without redeploying the stack, but it is still limited to retargeting already running servers rather than spawning or retiring them

## Phase 3 Acceptance Boundary

- current Phase 3 acceptance evidence combines:
  - desired/observed/reconciliation plus request/execution/realization/adapter/runtime-assignment control-plane proof through `verify_admin_api.py`, `verify_admin_auth.py`, and `verify_admin_read_only.py`
  - topology-aware runtime closure proof through `verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance`
- with the runtime-assignment live scale-out path now proven on the default stack, Phase 3 is considered closed for the current runtime scope because:
  - startup manifests are no longer the sole supported realization path
  - continuity, world residency, owner handoff, migration handoff, and one live desired-topology scale-out path are now reproducible supported runtime paths
- remaining work such as elastic process spawn/retire and autoscaling still exists, but it is no longer treated as a Phase 3 blocker; it belongs to later engine/package/release-evidence work.
