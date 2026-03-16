# MMORPG Desired Topology Contract

This document defines the future desired-topology and orchestration boundary for the MMORPG runtime.
It is a design contract only. It is not implemented as a live runtime/admin API in the current system.

## Purpose

- keep current startup-only topology manifests from becoming the long-term scaling contract
- separate desired topology from observed topology and from world lifecycle policy
- define the future orchestration tranche without binding the design to a Docker-only or Kubernetes-only control plane

## Current Boundary

- current supported topology control is startup-only:
  - `docker/stack/topologies/*.json`
  - `scripts/deploy_docker.ps1 -TopologyConfig <path>`
- current manifests are concrete local/proof artifacts
- world lifecycle policy remains a separate operator/runtime routing contract

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
  - revisioned desired-topology storage
  - observed-topology read models
  - reconciliation status
  - pool-oriented scaling APIs
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
