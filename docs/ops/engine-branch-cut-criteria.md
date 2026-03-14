# Engine Genre Branch-Cut Criteria

This document records the Phase 5 branch preparation decision for the `engine-readiness-baseline` branch.

The current baseline decision is now `ready to branch`; see `docs/ops/engine-readiness-decision.md`.

## Shared Prerequisites For Any Genre Branch

Open either `engine-roadmap-fps` or `engine-roadmap-mmorpg` only while all shared prerequisites below remain satisfied.

Current execution note:

- the accepted baseline is currently being exploited through the narrower capability-first branch `engine-roadmap-session-continuity`
- that branch exists because the stack is still chat/control oriented, so a direct genre-branded branch would overstate the current scope
- the first `engine-roadmap-session-continuity` tranche is now complete; the remaining gate is review/merge rather than more shared continuity work on the same branch

### 1. Common baseline blockers are closed

Required:

- overload/login-collapse blocker is fixed or reclassified with convincing bounded-failure evidence
- worker Redis degraded-state visibility gap is closed or replaced by an accepted alternative signal

Reference:

- `docs/ops/engine-readiness-decision.md`

Current status:

- satisfied by `build/engine-readiness/20260314-024138-redis-remediation/`
- satisfied by `build/engine-readiness/20260314-025202-overload-remediation-v3/`

### 2. The affected evidence runs are rerun and accepted

Minimum rerun set:

- overload/backpressure rehearsal
- targeted Redis outage/recovery verification for worker degraded-state signaling

The reruns must leave fresh artifacts and update the checkpoint ledger.

Current status:

- satisfied

### 3. The common baseline conclusion is updated to `ready to branch`

Required:

- `docs/ops/engine-readiness-baseline.md` reflects a non-blocked baseline
- the branch decision record is updated accordingly

Current status:

- satisfied

### 4. The target branch starts from a narrow charter

Each genre branch must start with:

- a first-tranche scope
- explicit non-goals
- a verification bar

The branch must not reopen unresolved common runtime questions that belong in the baseline.

## `engine-roadmap-fps` Branch-Cut Criteria

Open this branch only when all shared prerequisites above are met **and** the first FPS tranche is explicitly limited to FPS-specific engine work.

### First-Tranche Scope

- authoritative tick/runtime loop decisions
- snapshot/delta replication primitives
- interest-management primitives
- latency-compensation hooks
- gameplay-grade UDP/RUDP data-path hardening

### FPS Branch Must Not Start With

- unresolved common overload/backpressure ambiguity
- unresolved dependency-readiness signaling gaps
- MMORPG-style persistence/session-resume scope creep

### Minimum FPS Branch Entry Bar

- common baseline is already accepted
- current UDP/RUDP substrate remains green on attach success/fallback/OFF control scenarios
- the FPS branch charter names the first gameplay-traffic proof target that goes beyond the current attach-only transport evidence

## `engine-roadmap-mmorpg` Branch-Cut Criteria

Open this branch only when all shared prerequisites above are met **and** the first MMORPG tranche is explicitly limited to MMORPG-specific engine work.

### First-Tranche Scope

- long-lived session resume/reconnect semantics
- zone/shard/world lifecycle support
- persistence/recovery orchestration beyond the current chat/write-behind stack
- background task and state handoff guarantees

### MMORPG Branch Must Not Start With

- unresolved common overload/backpressure ambiguity
- unresolved dependency-readiness signaling gaps
- FPS gameplay replication or lag-compensation work mixed into the first tranche

### Minimum MMORPG Branch Entry Bar

- common baseline is already accepted
- current restart/recovery evidence is accepted as sufficient for bounded partial failure
- the MMORPG branch charter names the first session/persistence continuity proof target that goes beyond the current chat/control-path model

## Preferred First Branch After Baseline Closure

- preferred order: `engine-roadmap-mmorpg` first

## Reason

- The current stack is stronger in TCP control flow, readiness/lifecycle, Redis/Postgres dependency handling, write-behind, and admin/control-plane behavior than it is in gameplay-grade realtime transport.
- That means the present architecture is already closer to MMORPG-style backend concerns than to FPS-style authoritative realtime simulation.
- FPS work will almost certainly reopen the transport/runtime model more aggressively, especially around gameplay-frequency UDP/RUDP semantics.

## Current Status

- `engine-roadmap-session-continuity`: first tranche complete and merged
- `engine-roadmap-fps`: not cut yet
- `engine-roadmap-mmorpg`: active branch cut from merged continuity head
- `preferred first genre branch once the continuity tranche closes`: `engine-roadmap-mmorpg`
- `current operational step`: execute the narrow MMORPG first tranche around world admission/residency
