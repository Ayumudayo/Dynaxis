# MMORPG World Residency And Ownership Contract

This document records the current world admission, residency, owner-boundary, and lifecycle-policy contract now merged into `main`.
The earlier branch-start, tranche-closure, and world-lifecycle charter notes are absorbed here.

## Scope

- durable world residency is layered on top of the continuity substrate
- world-scoped owner authority and lifecycle policy are part of the current runtime contract
- startup-time topology selection for local/proof stacks is part of the supported surface
- the following remain out of scope:
  - live scale out/in semantics
  - autoscaling
  - active owner transfer choreography
  - live zone migration
  - gameplay simulation state continuity
  - combat/world replication

## Ownership

### Gateway

- resume locator hints include `world_id`
- `world_id` comes from the backend registry tag convention `world:<id>`
- if exact resume alias binding is missing, selector fallback can still constrain reconnect toward the same world/shard boundary

### Server

- server owns durable world residency for a logical session
- server owns the current world owner boundary via `SERVER_INSTANCE_ID`
- room continuity is subordinate to world continuity and owner continuity
- if world residency is missing, or owner continuity is missing/mismatched, restore falls back to `default world + lobby`

### Storage

- world residency uses a dedicated continuity Redis key
- world owner authority uses a dedicated continuity Redis key keyed by `world_id`
- world lifecycle policy uses `dynaxis:continuity:world-policy:<world_id>` with:
  - `draining=0|1`
  - `replacement_owner_instance_id=<instance_id>`
- world/owner/room keys share the lease-shaped TTL window

## World Lifecycle Policy

### Control plane

- `admin_app` exposes world lifecycle policy in:
  - `GET /api/v1/worlds`
  - `PUT /api/v1/worlds/{world_id}/policy`
  - `DELETE /api/v1/worlds/{world_id}/policy`
- the control plane answers:
  - which instance currently owns a world
  - whether the world is draining
  - whether a replacement owner target is declared
- policy writes go directly to the authoritative Redis world-policy key

### Runtime decisions

- fresh admission excludes a draining owner from new backend selection unless it is already the declared replacement owner
- resume routing applies the same draining-owner exclusion rule
- server-side restore requires:
  - persisted world residency
  - owner continuity matching the current backend
  - if draining, the declared replacement owner must already be the current backend
- otherwise the session falls back to the backend-safe default world plus `lobby`

## Current Topology Boundary

- supported topology control is still startup-only
- `docker/stack/topologies/*.json` are concrete startup manifests for local/proof use
- default Docker topology remains:
  - `server-1 -> world:starter-a`
  - `server-2 -> world:starter-b`
- same-world proof topology remains a separate startup manifest
- background reconciliation and automatic owner mutation are not part of the current runtime

## Resume Rules

- fresh login assigns `world_id` from:
  - `WORLD_ADMISSION_DEFAULT`, if set
  - otherwise the first `world:<id>` tag from `SERVER_TAGS`
  - otherwise `default`
- fresh login persists:
  - world residency
  - world owner authority
  - room continuity
- resume restores `world_id` and room only when residency and owner continuity are valid and any draining policy is already honored
- fallback reasons remain:
  - `missing_world`
  - `missing_owner`
  - `owner_mismatch`
  - `draining_replacement_unhonored`

## Observable Signals

- login response includes `world_id`
- server metrics expose world write/restore/fallback counters
- gateway metrics expose world-policy filter and replacement-selection counters
- admin metrics expose world-policy write/clear counters

## Current Proof Targets

- `python tests/python/verify_stack_topology_generator.py`
- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-missing-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-reassignment`
- `python tests/python/verify_admin_api.py`
- `python tests/python/verify_admin_auth.py`
- `python tests/python/verify_admin_read_only.py`

## Related Contract

- future desired topology and orchestration work remains defined by `docs/ops/mmorpg-desired-topology-contract.md`
