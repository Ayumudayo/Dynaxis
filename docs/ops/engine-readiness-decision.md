# Engine Readiness Baseline Decision

This document records the accepted Phase 4 decision for the `engine-readiness-baseline` branch.

Raw evidence for each rehearsal lives under `build/engine-readiness/<run_id>/`.
The checkpoint ledger is tracked in `docs/ops/engine-readiness-baseline.md`.

## Decision Date

- 2026-03-14

## Inputs

- `docs/ops/engine-readiness-baseline.md`
- `build/engine-readiness/20260312-0228-control-baseline/`
- `build/engine-readiness/20260314-0150-redis-recovery/`
- `build/engine-readiness/20260314-0155-postgres-recovery/`
- `build/engine-readiness/20260314-0212-gateway-restart/`
- `build/engine-readiness/20260314-0216-server-restart/`
- `build/engine-readiness/20260314-0219-worker-restart/`
- `build/engine-readiness/20260314-0223-overload-rehearsal/`
- `build/engine-readiness/20260314-024138-redis-remediation/`
- `build/engine-readiness/20260314-025202-overload-remediation-v3/`

## Resolved Common Blockers

### 1. Worker Redis degraded-state visibility is now explicit

Evidence:

- `build/engine-readiness/20260314-024138-redis-remediation/summary/result.md`
- worker readiness path:
  - pre: `200 ready`
  - during outage: `503 not ready: deps=redis`
  - post-recovery: `200 ready`
- worker metrics:
  - `runtime_dependency_ready{name="redis",required="true"} 0` during outage
  - `wb_redis_unavailable_total` increments during outage
  - `wb_reclaim_error_total` continues to expose reclaim-side Redis failures

Interpretation:

- The earlier Redis caveat on `wb_worker` was a real observability gap.
- The worker now advertises Redis degradation through the same dependency/readiness surfaces used by gateway/server.
- This blocker is closed for branch-cut purposes.

### 2. The overload/login-collapse blocker was an invalid harness result, and the accepted rerun passes

Evidence:

- initial failing run: `build/engine-readiness/20260314-0223-overload-rehearsal/summary/result.md`
- accepted rerun: `build/engine-readiness/20260314-025202-overload-remediation-v3/summary/result.md`
- loadgen remediation:
  - concurrent runs no longer reuse the same login IDs; usernames are now suffixed with the run seed
- accepted rerun aggregate:
  - `192` connected sessions
  - `192` authenticated sessions
  - `192` joined sessions
  - `0` login failures
  - `0` total errors

Why the original blocker was invalid:

- The original `8 x steady_chat` rehearsal launched eight separate loadgen processes with the same `login_prefix` and the same per-process session indices.
- That reused the same usernames (`steady_chat_0..23`) across concurrent runs.
- One run could claim those names; the others failed as duplicate-login attempts.
- That failure shape was not an engine overload signal, so it could not remain a common-runtime blocker.

Additional runtime tightening that stays valuable:

- Gateway equal-load backend selection now incorporates gateway-local optimistic load and deterministic tie-break behavior.
- In the accepted rerun both backends handled traffic (`server-1` and `server-2` both showed inline dispatch activity), so the stack no longer exhibits the single-backend pinning seen during investigation.

Interpretation:

- The accepted overload rehearsal now demonstrates bounded, healthy behavior at the target stress shape.
- The previous blocker is closed.

## Remaining Deferable Caveats

These are real follow-up items, but they do not block a genre branch split from the common baseline.

### 1. Transparent in-flight session continuity across gateway/server restart

Evidence:

- gateway restart live soak: `24` disconnects / `24` errors
- server restart live soak: `26` disconnects / `26` errors

Why this can defer:

- The current baseline requires bounded failure and automatic recovery, not seamless continuity for every in-flight session.
- The stack recovered automatically and new traffic stayed routable.

Likely future owner:

- mostly `engine-roadmap-mmorpg`
- partially shared engine follow-up if reconnect semantics are promoted more broadly

### 2. Worker backlog-depth visibility during restart remains weaker than ideal

Evidence:

- worker restart rehearsal still showed backlog recovery mainly through flush logs and batch sizes
- sampled `wb_pending` did not spike strongly in the retained window

Why this can defer:

- The current baseline already proves bounded worker restart recovery and successful backlog drain.
- The remaining gap is visibility quality, not correctness of recovery behavior.

Likely future owner:

- shared engine/runtime follow-up

### 3. Gameplay-grade UDP/RUDP transport maturity beyond the current attach/fallback proof

Why this can defer:

- The common baseline only requires bounded transport substrate behavior.
- Gameplay-frequency replication semantics remain genre-specific follow-up work.

Likely future owner:

- `engine-roadmap-fps`

## Historical Baseline Conclusion

- `ready to branch`: yes
- `baseline decision`: ready to split once the first branch charter is chosen
- `historical preferred first genre branch after the continuity tranche`: `engine-roadmap-mmorpg`

Reason:

- Dependency outage/recovery and process-restart rehearsals are already bounded and recover automatically.
- The worker Redis signaling gap is closed.
- The overload blocker is closed after correcting the concurrent-run identity collision in loadgen and rerunning the rehearsal with clean results.
- The remaining caveats are narrower restart/backlog-visibility concerns that can be carried into later branch-specific work without invalidating the shared baseline.
- Because the current runtime is still fundamentally a chat/control stack, the first post-baseline branch is intentionally capability-first (`engine-roadmap-session-continuity`) rather than a broad genre-branded MMORPG branch.

## Current Downstream Ownership

This document preserves the accepted shared-baseline conclusion. It does not act as a live branch tracker.

- transparent in-flight continuity across gateway/server restart
  - downstream owner: session-continuity / MMORPG-oriented follow-up
- worker backlog-depth visibility quality
  - downstream owner: shared engine/runtime follow-up if it becomes operationally necessary
- gameplay-grade UDP/RUDP transport maturity beyond attach/fallback proof
  - downstream owner: FPS-oriented follow-up

The accepted March 14, 2026 conclusion remains unchanged unless a later branch produces evidence that invalidates the shared baseline itself.

## Guardrail For Branch Cut

Open the next genre branch only if all of the following remain true:

1. The accepted overload rehearsal remains green under the identity-safe loadgen shape.
2. Worker Redis dependency/readiness signaling remains explicit in the accepted remediation replay.
3. The chosen genre branch starts from a narrow first tranche and does not reopen already-closed common baseline questions as implicit work.
