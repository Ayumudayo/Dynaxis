# MMORPG World Residency And Ownership Contract

This document records the current world admission, residency, owner-boundary, and lifecycle-policy contract now merged into `main`.
The earlier branch-start, tranche-closure, and world-lifecycle charter notes are absorbed here.

## Scope

- durable world residency is layered on top of the continuity substrate
- world-scoped owner authority and lifecycle policy are part of the current runtime contract
- operator-driven live owner transfer commit is part of the current runtime contract
- operator-declared cross-world migration envelope is part of the current runtime contract
- startup-time topology selection for local/proof stacks is part of the supported surface
- live reassignment of an idle running server into a leased desired-topology pool is now part of the supported surface
- the following remain out of scope:
  - elastic process spawn/retire
  - autoscaling
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
  - `GET /api/v1/worlds/{world_id}/drain`
  - `PUT /api/v1/worlds/{world_id}/drain`
  - `DELETE /api/v1/worlds/{world_id}/drain`
  - `GET /api/v1/worlds/{world_id}/transfer`
  - `PUT /api/v1/worlds/{world_id}/transfer`
  - `DELETE /api/v1/worlds/{world_id}/transfer`
  - `GET /api/v1/worlds/{world_id}/migration`
  - `PUT /api/v1/worlds/{world_id}/migration`
  - `DELETE /api/v1/worlds/{world_id}/migration`
- the control plane answers:
  - which instance currently owns a world
  - whether the world is draining
  - whether a replacement owner target is declared
- named owner transfer can now:
  - declare the replacement target through the lifecycle policy
  - commit the continuity world owner key to the replacement owner without waiting for a fresh login side effect
  - clear transfer policy after the owner boundary has moved
- named world drain can now:
  - declare drain intent on top of the same lifecycle policy primitive
  - report live remaining-session progress from observed world inventory
  - distinguish `draining_sessions` from `drained` without requiring a fresh-login or owner-commit side effect
  - expose drain orchestration state so operators can see whether the next supported closure action is waiting, stabilizing a replacement target, committing owner transfer, awaiting migration readiness, or clearing policy
- named world migration can now:
  - declare `source world -> target world owner` intent separately from desired topology and lifecycle policy
  - carry opaque app-defined payload references without interpreting them in core/runtime policy
  - let continuity resume restore onto the target world when the source world is draining and the target owner matches the resumed backend
- policy writes go directly to the authoritative Redis world-policy key

### Runtime decisions

- fresh admission excludes a draining owner from new backend selection unless it is already the declared replacement owner
- resume routing applies the same draining-owner exclusion rule
- server-side restore requires:
  - persisted world residency
  - owner continuity matching the current backend
  - if draining, the declared replacement owner must already be the current backend
- when source-world restore cannot stay on the original world because it is draining, a migration envelope may restore onto the target world if:
  - the source world is draining
  - the target owner matches the current backend
  - the current backend already hosts the target world or the target world owner key matches the current backend
- `server_app` currently defines one app-local payload seam on top of that generic envelope:
  - `payload_kind="chat-room-v1"` means `payload_ref=<target room>`
  - on successful migration restore, the resumed room boundary moves to that target room instead of using generic `preserve_room`
- otherwise the session falls back to the backend-safe default world plus `lobby`
- current supported drain-closure path is:
  - declare named drain
  - wait until active sessions on the source owner reach zero
  - commit same-world owner transfer or satisfy migration readiness
  - observe `drain.orchestration.next_action=clear_policy`
  - clear the drain policy explicitly

## Current Topology Boundary

- supported topology control is startup bootstrap plus live runtime reassignment of idle running servers
- `docker/stack/topologies/*.json` are concrete startup manifests for local/proof use
- default Docker topology remains:
  - `server-1 -> world:starter-a`
  - `server-2 -> world:starter-b`
- same-world proof topology remains a separate startup manifest
- supported proof mapping is now:
  - default topology: drain progress, drain-to-migration closure, cross-world migration handoff, app-local target-room migration handoff
  - same-world topology: explicit owner transfer commit, drain-to-owner-transfer closure
- background reconciliation and automatic owner mutation are not part of the current runtime
- Phase 3 acceptance is now considered closed for the current runtime scope:
  - runtime closure paths are repeatably proven
  - one live scale-out path can now retarget an idle running server into a desired pool without redeploying the stack
  - elastic spawn/retire and autoscaling still remain outside the current supported runtime and are not part of the accepted Phase 3 contract

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
- server metrics also expose app-local migration room handoff counters
  - `chat_continuity_world_migration_payload_room_handoff_total`
  - `chat_continuity_world_migration_payload_room_handoff_fallback_total`
- server registry heartbeat now also honors the runtime-assignment document when present, so `GET /api/v1/topology/observed` and `GET /api/v1/topology/actuation` reflect the live reassignment within the normal registry heartbeat window
- admin metrics also expose world-drain write/clear counters
- gateway metrics expose world-policy filter and replacement-selection counters
- admin metrics expose world-policy and world-transfer write/clear/owner-commit counters
- admin metrics also expose world-migration write/clear counters

## Current Proof Targets

- `python tests/python/verify_stack_topology_generator.py`
- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-missing-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-reassignment`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-progress`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-transfer-closure`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-migration-closure`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-transfer-commit`
- `python tests/python/verify_session_continuity_restart.py --scenario world-migration-handoff`
- `python tests/python/verify_session_continuity_restart.py --scenario world-migration-target-room-handoff`
- `python tests/python/verify_admin_api.py`
- `python tests/python/verify_admin_auth.py`
- `python tests/python/verify_admin_read_only.py`
- `verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance` is the preferred repeatable acceptance-proof entrypoint when the goal is to exercise the supported Phase 3 runtime paths, including runtime-assignment live scale-out, instead of one scenario at a time
- quantified Phase 5 handoff/restart evidence ownership is tracked in `docs/ops/quantified-release-evidence.md`

## Related Contract

- desired topology, actuation, and orchestration layers remain defined by `docs/ops/mmorpg-desired-topology-contract.md`
