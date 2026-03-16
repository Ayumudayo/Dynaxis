# Engine Roadmap Session Continuity First-Tranche Closure

This document records the closure decision for the first tranche on `engine-roadmap-session-continuity`.

## Decision Date

- 2026-03-14

## Inputs

- `docs/ops/engine-roadmap-session-continuity-charter.md`
- `docs/ops/session-continuity-contract.md`
- `build/session-continuity/20260314-193004-restart-rehearsal/summary/result.md`
- `build/session-continuity/20260314-195212-locator-handoff-proof/summary/result.md`

## What This Tranche Was Supposed To Prove

The first tranche existed to move the stack from bounded restart recovery to resumable logical session continuity without pretending the runtime was already a full MMORPG server.

The required proof points were:

- explicit resume/reconnect contract
- explicit continuity state ownership across gateway/server/storage
- bounded restart continuity proof for gateway and server restart
- minimal shard/world locator boundary
- observable continuity handoff writes/restores

## What Was Completed

### 1. Resumable logical session contract

- `LoginRes` now returns:
  - `logical_session_id`
  - `resume_token`
  - `resume_expires_unix_ms`
  - `resumed`
- resume login uses `resume:<token>`
- invalid resume tokens are rejected instead of silently downgrading to fresh login

### 2. Continuity state ownership

- server owns lease issuance and validation
- Redis continuity keys mirror the last lightweight room/location
- gateway owns hashed resume alias routing and persists a minimal locator hint

### 3. Restart-aware reconnect proof

- gateway restart rehearsal passed
- server restart rehearsal passed
- exact sticky-key loss locator fallback rehearsal passed

### 4. Minimal shard/world locator boundary

- the gateway now persists:
  - `backend_instance_id`
  - `role`
  - `game_mode`
  - `region`
  - `shard`
- if the exact resume alias binding is gone, reconnect is first narrowed by the locator hint before any global fallback

### 5. Continuity handoff observability

- gateway metrics now include:
  - `gateway_resume_locator_bind_total`
  - `gateway_resume_locator_lookup_hit_total`
  - `gateway_resume_locator_lookup_miss_total`
  - `gateway_resume_locator_selector_hit_total`
  - `gateway_resume_locator_selector_fallback_total`
- server metrics now include:
  - `chat_continuity_lease_issue_total`
  - `chat_continuity_lease_issue_fail_total`
  - `chat_continuity_lease_resume_total`
  - `chat_continuity_lease_resume_fail_total`
  - `chat_continuity_state_write_total`
  - `chat_continuity_state_write_fail_total`
  - `chat_continuity_state_restore_total`
  - `chat_continuity_state_restore_fallback_total`

## Verification Summary

- `python tests/python/verify_chat.py`
- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_session_continuity_restart.py`
- `python tests/python/verify_admin_api.py`

All of the above passed in the accepted sequential proof run used for closure.

## Tranche Conclusion

- `first tranche status`: complete
- `branch status`: ready for review/merge
- `next recommended branch after merge`: `engine-roadmap-mmorpg`

Reason:

- The original first-tranche charter goals are now satisfied.
- The current branch has reached the point where adding more scope here would blur the capability-first closure boundary.
- The next useful work is no longer generic session continuity; it is MMORPG-specific world/session orchestration built on top of the continuity substrate now in place.

## Explicit Carry-Forward

Closing this tranche does not mean the engine now has full MMORPG behavior.

It only means the shared continuity substrate is strong enough that the next branch can legitimately start with narrower MMORPG-specific concerns such as:

- world admission and residency boundaries
- shard/world-scoped background ownership
- persistence orchestration beyond room continuity

It does not yet prove:

- live zone migration
- gameplay simulation state continuity
- combat replication or authoritative world simulation
