# MMORPG Desired Topology Contract

This document defines the future desired-topology model for the MMORPG branch.

It is a design contract only. It is not implemented as a runtime/admin API in the current tranche.

## Purpose

- define the canonical future desired-topology model before any live scaling work starts
- keep the current startup-only topology manifests from accidentally becoming the long-term scaling contract
- separate desired topology from observed topology and from world lifecycle policy

## Current Boundary

- current supported topology control is startup-only:
  - `docker/stack/topologies/*.json`
  - `scripts/deploy_docker.ps1 -TopologyConfig <path>`
- current startup manifests are concrete local/proof artifacts
- they are not the desired-topology contract

## Canonical Model

The future desired-topology contract is pool-based.

It is not concrete-instance-based.

Desired state declares:

- which `world_id` / `shard` pools should exist
- how many replicas each pool should have
- optional placement/capacity hints

Desired state does **not** declare:

- concrete `instance_id`
- concrete container/pod names
- concrete host ports

## Minimal Contract Shape

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

## Field Semantics

- `topology_id`
  - stable logical identifier for the desired topology set
  - example intent: `starter-worlds-primary`
- `revision`
  - monotonic desired-state revision used by future orchestrators/reconcilers
  - semantic versioning is not required
- `world_id`
  - logical world identifier already used by the current world residency contract
- `shard`
  - routing/placement boundary paired with the world pool
- `replicas`
  - desired count of healthy serving instances in the pool
  - this is the scaling field, not an observed count
- `capacity_class`
  - optional qualitative hint for pool sizing/placement
  - examples might later include `small`, `standard`, `high-memory`
- `placement_tags`
  - optional orchestrator-facing placement hints
  - treated as opaque labels by this contract

## Desired vs Observed Topology

Desired topology is pool-based.

Observed topology is concrete-instance-based.

Observed topology is the only place where these appear:

- `instance_id`
- actual host / endpoint
- actual placement result
- actual readiness / health
- actual world owner/read-model state

This split is intentional:

- desired state answers "what pool shape do we want?"
- observed state answers "which instances actually exist?"

## Relationship To World Lifecycle Policy

World lifecycle policy remains a separate contract.

Desired topology must not directly absorb:

- `draining`
- `replacement_owner_instance_id`
- owner continuity keys

Reason:

- world lifecycle policy is an operator/runtime routing contract
- desired topology is a capacity/placement contract

Future implementations may coordinate them, but the contracts remain separate.

## Relationship To Startup Topology Manifests

Current startup manifests remain valid for:

- local development
- proof topology selection
- deterministic validation stacks

They remain concrete-instance manifests.

They should not be renamed to `desired topology`.

If a future implementation needs both surfaces, keep both:

- startup topology manifest: concrete, local/proof, startup-only
- desired topology contract: pool-based, orchestration-facing, revisioned

## Explicit Non-Goals

- no concrete `instance_id` in desired topology
- no host port declarations
- no Docker-only or Kubernetes-only semantics in the contract itself
- no requirement that desired topology be stored in Redis
- no live scaling API in this document
