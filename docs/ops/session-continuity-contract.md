# Session Continuity Contract

This document records the current session-continuity contract now merged into `main`.
The original branch-start and tranche-closure notes are absorbed here; this file is the live source of truth.

## Scope

- continuity covers authenticated logical sessions on top of the existing chat/control stack
- the persisted continuity surface stays intentionally narrow:
  - effective user identity
  - logical session ID
  - resume lease expiry
  - last lightweight room/location
- gameplay-state recovery, zone migration, and realtime FPS transport continuity remain out of scope

## Ownership

### Server

- `server_app` owns continuity lease issuance and validation
- fresh authenticated login issues:
  - `logical_session_id`
  - `resume_token`
  - `resume_expires_unix_ms`
  - `resumed`
- resume login uses the token format `resume:<token>`
- a valid persisted lease restores effective user identity and last continuity room
- an invalid or stale resume token is rejected with `UNAUTHORIZED`

### Gateway

- `gateway_app` never trusts the claimed reconnect user name on resume
- it hashes the raw resume token into `resume-hash:<sha256(token)>` and uses that alias for sticky reconnect selection
- after a successful login response, the gateway stores the alias plus a minimal locator hint through `SessionDirectory`
- the locator hint contains:
  - `role`
  - `game_mode`
  - `region`
  - `shard`
  - `backend_instance_id`
- if the exact alias binding is missing, the locator hint constrains fallback selection before global least-load fallback

### Storage

- the continuity lease owner remains the existing `sessions` repository path
- last lightweight room/location is mirrored to Redis continuity keys
- join/leave/login refresh both the room snapshot TTL and the lease window
- the gateway persists the resume locator hint under a sibling Redis key with the same lease-shaped TTL

## Decision Rules

### Fresh login

- no `resume:` prefix
- normal auth path
- issue a new continuity lease when continuity is enabled and the login has a persisted user identity

### Resume login

- gateway routes by hashed resume alias
- server validates the persisted lease
- if validation succeeds:
  - restore logical identity and last continuity room
  - return `resumed=true`
- if validation fails:
  - reject the login
  - do not silently downgrade to a fresh login

## Restart Expectations

- gateway restart:
  - a client may reconnect through a different surviving gateway
  - the resume alias survives through `SessionDirectory`
  - if the exact alias binding is gone, locator metadata still narrows reconnect toward the same shard boundary
- server restart:
  - persisted continuity lease remains valid inside the lease TTL
  - resume succeeds deterministically from persisted continuity state after backend return

## Current Proof Targets

- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline`
- `python tests/python/verify_session_continuity_restart.py --scenario gateway-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario server-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline` is the preferred repeatable restart/recovery entrypoint when Phase 5 evidence should be gathered as one sequence instead of per-scenario manual runs

## Downstream Relationship

- world residency/lifecycle work extends this contract but does not replace it
- continuity remains the base restore layer for the broader MMORPG runtime contract
