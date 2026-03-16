# MMORPG World Residency And Ownership Contract

This document records the current world admission, residency, and owner-boundary contract on `engine-roadmap-mmorpg`.

## Scope

- This slice adds durable world residency plus a world-scoped owner boundary on top of the continuity substrate.
- It now also adds a minimal world lifecycle policy boundary for drain/reassignment declarations.
- It now also fixes the startup-time topology selection boundary for local/proof stacks.
- It still does not implement:
  - live scale out/in semantics
  - autoscaling
  - active owner transfer choreography
  - live zone migration
  - gameplay simulation state continuity
  - combat/world replication

## Ownership

### Gateway

- gateway resume locator hints now include `world_id`
- `world_id` is derived from the backend registry tag convention `world:<id>`
- if the exact resume alias binding is missing, selector fallback can still constrain reconnect toward the same world/shard boundary

### Server

- server owns durable world residency state for a logical session
- server also owns the current world-residency authority for its advertised world via `SERVER_INSTANCE_ID`
- the residency key is persisted separately from room continuity
- the owner key is persisted separately from both residency and room continuity
- room continuity is now subordinate to world continuity:
  - if world residency is restored, the room may be restored
  - if world residency is missing, room restore is not trusted and the session falls back to `lobby`
- room continuity is also subordinate to owner continuity:
  - if the persisted world owner matches the current backend owner, room restore may proceed
  - if the world owner is missing or mismatched, the session falls back to `default world + lobby`

### Storage

- world residency lives at a dedicated continuity Redis key
- world owner authority lives at a dedicated continuity Redis key keyed by `world_id`
- world lifecycle policy lives at `dynaxis:continuity:world-policy:<world_id>`
  - `draining=0|1`
  - `replacement_owner_instance_id=<instance_id>`
- room continuity still lives at its own continuity Redis key
- both keys share the lease-shaped TTL window

## World Lifecycle Policy

### Control plane

- `admin_app` exposes world lifecycle policy in:
  - `GET /api/v1/worlds`
  - `PUT /api/v1/worlds/{world_id}/policy`
  - `DELETE /api/v1/worlds/{world_id}/policy`
  - `world_scope.policy` for:
    - `GET /api/v1/instances`
    - `GET /api/v1/instances/{instance_id}`
    - `GET /api/v1/sessions/{client_id}`
    - `GET /api/v1/users`
- the control plane can now answer:
  - which instance currently owns a world
  - whether the world is marked draining
  - whether a replacement owner target is declared
- the control plane can also mutate that policy directly:
  - writes go straight to the authoritative Redis world-policy key
  - no signed fanout/admin publish path is used for this state

### Runtime decisions

- fresh admission:
  - gateway excludes a draining owner from new backend selection unless that instance is the declared replacement owner
- resume routing:
  - sticky resume alias and locator fallback selection obey the same draining-owner exclusion rule
- server-side restore:
  - world restore still requires owner continuity to match the current backend
  - if the world is draining, restore is additionally allowed only when the declared replacement owner is already the current backend
  - otherwise the session falls back to the backend-safe default world + `lobby`

### Current topology boundary

- this contract supports startup-only topology selection
- `docker/stack/topologies/*.json` are concrete startup topology manifests for local/proof use
- these manifests are not the future desired topology contract
- this slice does not add any background reconciliation loop
- this slice does not mutate world owner state automatically
- default Docker topology remains:
  - `server-1 -> world:starter-a`
  - `server-2 -> world:starter-b`
- same-world proof topology is now a separate startup manifest:
  - `server-1 -> world:starter-a`
  - `server-2 -> world:starter-b`
  - `server-3 -> world:starter-a`
- topology selection is startup-only:
  - `scripts/deploy_docker.ps1 -TopologyConfig <path>`
  - `scripts/run_full_stack_observability.ps1 -TopologyConfig <path>`
- live scale in/out and autoscaling semantics remain out of scope
- future design references:
  - `docs/ops/mmorpg-desired-topology-contract.md`
  - `docs/ops/mmorpg-scaling-orchestration-charter.md`

### Desired vs observed topology

- current startup topology manifests are concrete-instance declarations
  - they list exact `server-*` instances and host ports
  - they exist to start a known local/proof stack shape
- future desired topology is a separate pool-based concept
  - it declares `world/shard` pools and replica counts
  - it does not require concrete `instance_id`
- future observed topology is where concrete instances appear
  - concrete `instance_id`
  - actual placement/health
  - actual owner/read-model visibility

## Decision Rules

### Fresh login

- assign `world_id` from:
  - `WORLD_ADMISSION_DEFAULT`, if set
  - otherwise the first `world:<id>` tag from `SERVER_TAGS`
  - otherwise `default`
- persist both:
  - world residency
  - world owner authority
  - room continuity

### Resume login

- if the persisted world key exists:
  - and the persisted world owner matches the current backend owner:
    - and the world is not draining, or the declared replacement owner is already the current backend:
      - restore `world_id`
      - attempt room restore from the room continuity key
- if the persisted world key exists but the owner key is missing or mismatched:
  - fall back to the safe default world for the current backend
  - force room to `lobby`
- if the persisted world key exists and the owner matches but the world is draining toward a different replacement owner:
  - fall back to the safe default world for the current backend
  - force room to `lobby`
- if the persisted world key is missing:
  - fall back to the safe default world for the current backend
  - force room to `lobby`

### Fallback reason taxonomy

- `missing_world`
  - the persisted world residency key is absent or empty
- `missing_owner`
  - the persisted world owner key is absent or empty after world residency was found
- `owner_mismatch`
  - the persisted world owner key exists but does not match the current backend owner
- `draining_replacement_unhonored`
  - the world is marked draining and the current backend is not the declared replacement owner
  - this reason wins over owner drift in the hardening metrics because the policy boundary is the operator-visible cause of the fallback

## Observable Signals

- login response now includes `world_id`
- server metrics now expose:
  - `chat_continuity_world_write_total`
  - `chat_continuity_world_write_fail_total`
  - `chat_continuity_world_restore_total`
  - `chat_continuity_world_restore_fallback_total`
  - `chat_continuity_world_owner_write_total`
  - `chat_continuity_world_owner_write_fail_total`
  - `chat_continuity_world_owner_restore_total`
  - `chat_continuity_world_owner_restore_fallback_total`
  - `chat_continuity_world_restore_fallback_reason_total{reason="missing_world"}`
  - `chat_continuity_world_restore_fallback_reason_total{reason="missing_owner"}`
  - `chat_continuity_world_restore_fallback_reason_total{reason="owner_mismatch"}`
  - `chat_continuity_world_restore_fallback_reason_total{reason="draining_replacement_unhonored"}`
- gateway metrics now expose:
  - `gateway_world_policy_filtered_total{source="sticky"}`
  - `gateway_world_policy_filtered_total{source="candidate"}`
  - `gateway_world_policy_replacement_selected_total`
- admin metrics now expose:
  - `admin_world_policy_write_total`
  - `admin_world_policy_write_fail_total`
  - `admin_world_policy_clear_total`
  - `admin_world_policy_clear_fail_total`

## Explicit Non-Goals For This Contract

- no desired-topology Redis key
- no new admin endpoint for live scaling
- no reconciler loop inside `gateway_app`, `server_app`, or `admin_app`
- no scheduler that translates desired topology into instance create/remove actions
- no automatic merge between world policy and future desired topology

## Proof Targets

- generator / topology validation:
  - `python tests/python/verify_stack_topology_generator.py`
- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-missing-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-reassignment` (same-world proof topology only)
- `python tests/python/verify_admin_api.py`
- `python tests/python/verify_admin_auth.py`
- `python tests/python/verify_admin_read_only.py`
