# Engine Readiness Baseline Decision

This document records the Phase 4 decision for the `engine-readiness-baseline` branch.

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

## Common Engine Blockers Exposed By The Matrix

## Must Fix Before Any Genre Split

### 1. Overload/login collapse is real, but the intended backpressure signals do not explain it

Evidence:

- `build/engine-readiness/20260314-0223-overload-rehearsal/summary/result.md`
- aggregate overload run:
  - `192` connected sessions
  - `36` authenticated sessions
  - `156` login failures
  - `164` total errors

Why this blocks the baseline:

- The current baseline requires overload to degrade in a bounded and observable way.
- The stack did degrade without wedging, but the expected gateway queue/circuit/reject counters stayed flat while logins failed in bulk.
- That means the dominant common-runtime overload path is still not explained well enough for branch split.

Required outcome before branch split:

- either eliminate the login-collapse pattern at this load level
- or make the failure bounded and immediately explainable through the intended runtime metrics and rejection surfaces

### 2. Worker Redis degradation visibility is weaker than the gateway/server visibility story

Evidence:

- `build/engine-readiness/20260314-0150-redis-recovery/summary/result.md`
- gateway/server dropped to `503 not ready: deps=redis`
- worker logs showed Redis read/reclaim failures, but sampled `worker /readyz` and `runtime_dependency_ready{name="redis"}` did not flip during the outage window

Why this still matters:

- The current rubric says affected services should advertise degraded state observably.
- Gateway/server satisfy that expectation clearly.
- Worker recovery is visible in logs/counters, but the ready/dependency signal is still weaker than the rest of the stack.

Required outcome before branch split:

- strengthen worker degraded-state signaling
- or explicitly replace the missing ready/dependency signal with an accepted, documented alternative that closes the observability gap

## Explicitly Deferable Items

These items are real follow-up work, but they do not have to block the common branch split once the must-fix blockers above are resolved.

### 1. Transparent in-flight session continuity across gateway/server restart

Evidence:

- gateway restart live soak: `24` disconnects / `24` errors
- server restart live soak: `26` disconnects / `26` errors

Why this can defer:

- The current baseline promise only requires bounded failure and recovery, not seamless continuity for every in-flight session.
- The rehearsals recovered automatically and new traffic stayed routable.

Likely future owner:

- mostly `engine-roadmap-mmorpg`
- partially shared engine follow-up if reconnect semantics are promoted more broadly

### 2. Gameplay-grade UDP/RUDP transport maturity

Evidence:

- control baseline proves current attach success/fallback/OFF behavior
- the common baseline still does not promise high-frequency gameplay replication semantics

Why this can defer:

- the common baseline only needs measurable, bounded transport substrate behavior
- gameplay-grade realtime transport is a genre-specific step beyond the current baseline

Likely future owner:

- `engine-roadmap-fps`

### 3. Long-lived session resume, shard/zone lifecycle, and persistence orchestration above the current stack

Why this can defer:

- those are not required for the common baseline proof bar
- they should be scoped explicitly in the MMORPG branch instead of muddying the common blocker list

Likely future owner:

- `engine-roadmap-mmorpg`

## Baseline Conclusion

- `ready to branch`: no
- `baseline decision`: not ready to split into `engine-roadmap-fps` / `engine-roadmap-mmorpg`

Reason:

- The failure/recovery matrix is now explicit and largely positive for dependency loss and process restart.
- However, the branch still lacks one critical common-runtime proof: an overload/backpressure story that is both bounded and quickly explainable from the intended runtime signals.
- The worker Redis degraded-state visibility gap remains a secondary common-runtime caveat that should not be ignored while closing the main blocker.

## Reopen Condition For Branch Cut

Do not open the genre branches until all of the following are true:

1. The overload/backpressure blocker is either fixed or reclassified with convincing bounded-failure evidence and matching metrics.
2. Worker Redis degraded-state visibility is strengthened or explicitly replaced by an accepted alternative signal.
3. The branch-cut criteria for `engine-roadmap-fps` and `engine-roadmap-mmorpg` are written and accepted.
