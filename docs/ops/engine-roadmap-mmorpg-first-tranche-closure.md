# Engine Roadmap MMORPG First-Tranche Closure

This document records the closure decision for the first tranche on `engine-roadmap-mmorpg`.

## Decision Date

- 2026-03-14

## Inputs

- `docs/ops/engine-roadmap-mmorpg-charter.md`
- `docs/ops/mmorpg-world-residency-contract.md`
- `build/engine-roadmap-mmorpg/20260314-210313-world-admission-metadata/summary/result.md`
- `build/engine-roadmap-mmorpg/20260314-211817-world-residency-fallback/summary/result.md`
- `build/engine-roadmap-mmorpg/20260314-214249-world-owner-boundary/summary/result.md`
- PR `Ayumudayo/Dynaxis#15`

## What This Tranche Was Supposed To Prove

The first tranche existed to show that MMORPG-oriented world admission and residency boundaries could be layered on top of the continuity substrate without pretending the runtime already had full world orchestration.

The required proof points were:

- explicit world admission metadata
- durable world residency state separate from room continuity
- explicit owner boundary for world-scoped restore
- bounded restart/reconnect proof for world restore and fallback
- observable control-plane/runtime signals for residency handoff

## What Was Completed

### 1. World admission metadata

- gateway resume locator hints now persist `world_id`
- `world_id` is derived from backend registry tags via the `world:<id>` convention
- locator fallback still constrains reconnect toward the same shard/world boundary

### 2. Durable world residency

- `LoginRes` now includes `world_id`
- server persists world residency separately from room continuity
- missing world residency forces `default world + lobby` fallback

### 3. World owner boundary

- server persists `dynaxis:continuity:world-owner:<world_id>`
- owner identity is derived from `SERVER_INSTANCE_ID`
- room restore is trusted only when:
  - world residency exists
  - persisted world owner matches the current backend owner
- owner mismatch or missing owner forces `default world + lobby`

### 4. Recovery proof

- gateway restart rehearsal passed
- server restart rehearsal passed
- locator fallback rehearsal passed
- world residency fallback rehearsal passed
- world owner fallback rehearsal passed

### 5. Observability and control-plane visibility

- server metrics now expose world owner write/restore/fallback counters
- admin/control-plane instance payloads now expose `world_scope`:
  - `world_id`
  - `owner_instance_id`
  - `owner_match`
  - `source.owner_key`

## Verification Summary

- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_session_continuity_restart.py --scenario gateway-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario server-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
- `python tests/python/verify_chat.py`
- `python tests/python/verify_admin_api.py`

All of the above passed in the accepted sequential proof run used for closure.

## Tranche Conclusion

- `first tranche status`: complete
- `branch status`: merged once, ready for the next tranche PR
- `merged PR`: `Ayumudayo/Dynaxis#15`

Reason:

- The original first-tranche charter goals are satisfied.
- The branch now has enough explicit world admission/residency semantics to stop calling this only continuity work.
- The next useful work is no longer residency fallback itself; it is lifecycle orchestration and control-plane policy built on top of the now-visible world boundary.

## Explicit Carry-Forward

Closing this tranche does not mean the engine now has full MMORPG world orchestration.

It means only that the runtime now has a defensible first boundary for:

- world admission metadata
- durable world residency
- owner-gated restore
- control-plane visibility for that boundary

It still does not prove:

- live zone migration
- world drain / reassignment choreography
- gameplay/system simulation
- multi-world orchestration beyond owner/fallback visibility
