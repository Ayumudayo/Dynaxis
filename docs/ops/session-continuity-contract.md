# Session Continuity Contract

This document records the current capability-first session continuity contract on `engine-roadmap-session-continuity`.

## Scope

- The current continuity slice covers authenticated logical sessions on top of the existing chat/control stack.
- It does not promise arbitrary gameplay-state recovery, zone migration, or FPS-grade realtime transport continuity.
- The persisted continuity state is intentionally narrow:
  - effective user identity
  - logical session ID
  - resume lease expiry
  - last lightweight room/location

## Ownership

### Server

- `server_app` owns continuity lease issuance and validation.
- Fresh authenticated login issues a persisted logical session lease and returns:
  - `logical_session_id`
  - `resume_token`
  - `resume_expires_unix_ms`
  - `resumed`
- Resume login uses the token format `resume:<token>`.
- A valid persisted lease restores the effective user and last continuity room.
- An invalid or stale resume token is rejected with `UNAUTHORIZED`.

### Gateway

- `gateway_app` never trusts the claimed reconnect user name on resume.
- On a resume attempt, the gateway hashes the raw resume token into `resume-hash:<sha256(token)>`.
- The hashed alias is the routing key used during reconnect selection.
- After a successful login response, the gateway extracts `resume_token` from `MSG_LOGIN_RES` and binds the hashed alias to the backend instance that issued the lease.
- The alias is stored through the existing `SessionDirectory` so it survives local gateway restarts.
- The gateway also stores a minimal locator hint next to the alias:
  - `role`
  - `game_mode`
  - `region`
  - `shard`
  - `backend_instance_id`
- If the exact alias -> backend sticky binding disappears before reconnect, the gateway uses the locator hint to constrain fallback selection before falling back to the global least-load path.

### Storage

- The persisted lease owner is the existing `sessions` repository path.
- The last lightweight room/location is mirrored to Redis continuity keys.
- Join/leave/login updates refresh the continuity room snapshot TTL alongside the lease window.
- The gateway persists the resume locator hint under a sibling Redis key with the same lease-shaped TTL window.

## Decision Rules

### Fresh login

- no `resume:` prefix
- normal auth path
- issue a new continuity lease when continuity is enabled and the login has a persisted user identity

### Resume login

- token format: `resume:<token>`
- gateway routes by hashed resume alias
- server validates the persisted lease
- if validation succeeds:
  - restore effective user identity
  - restore logical session ID
  - restore last continuity room
  - return `resumed=true`
- if validation fails:
  - reject the login
  - do not silently downgrade to a fresh login

## Restart Expectations

### Gateway restart

- A client may reconnect through a different surviving gateway.
- The resume alias remains available through `SessionDirectory`.
- If the exact alias binding is missing, locator metadata still narrows the reconnect target back toward the same shard boundary.
- The reconnect path should preserve logical identity and last room.

### Server restart

- After the restarted backend returns to service, the persisted continuity lease remains valid inside the lease TTL.
- Resume succeeds deterministically from persisted continuity state, even though the original transport session was lost.

## Proof Targets

- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_session_continuity_restart.py --scenario gateway-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario server-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
