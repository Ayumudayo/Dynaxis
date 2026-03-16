# Engine Readiness Baseline Checkpoints

This document is the closed checkpoint ledger for the accepted `engine-readiness-baseline` branch.

The detailed working runbooks and raw artifact notes live under local-only `tasks/` and ignored `build/engine-readiness/`.
This file exists so the accepted common-baseline evidence remains visible after downstream branches move on.

## Baseline Status

- `accepted shared-proof baseline`: yes
- `ready to branch`: yes
- `preferred first downstream genre branch`: `engine-roadmap-mmorpg`
- remaining restart/backlog caveats are deferred follow-up, not active baseline blockers

## Historical Execution Rule

- Checkpoints were executed strictly in runbook order.
- Each completed checkpoint was kept small enough to be committed independently.
- The run root under `build/engine-readiness/<run_id>/` is the raw evidence bundle for the matching checkpoint summary here.

## Phase 3 Checkpoints

| Order | Checkpoint | Run Root | Verdict | Key Outcome |
| --- | --- | --- | --- | --- |
| 1 | Control baseline rerun | `build/engine-readiness/20260312-0228-control-baseline/` | pass | TCP/UDP/RUDP control comparator stayed clean enough to begin failure/recovery rehearsals. |
| 2 | Redis outage/recovery | `build/engine-readiness/20260314-0150-redis-recovery/` | pass with caveat | gateway/server degraded visibly and recovered automatically; worker Redis outage signaling stayed weaker than gateway/server signaling. |
| 3 | Postgres outage/recovery | `build/engine-readiness/20260314-0155-postgres-recovery/` | pass | server/worker advertised DB degradation cleanly, worker retry/reconnect signals were explicit, and recovery completed automatically. |
| 4 | gateway restart during live traffic | `build/engine-readiness/20260314-0212-gateway-restart/` | pass with caveat | stack recovered and new traffic stayed routable, but the live soak recorded `24` disconnects / `24` errors for sessions on the restarted gateway. |
| 5 | server restart during live traffic | `build/engine-readiness/20260314-0216-server-restart/` | pass with caveat | stack recovered and probes kept passing, but the live soak recorded `26` disconnects / `26` errors for sessions routed to the restarted backend. |
| 6 | worker restart during backlog processing | `build/engine-readiness/20260314-0219-worker-restart/` | pass with caveat | user-facing chat stayed clean and the worker drained backlog after restart, but backlog visibility came from flush logs more than from `wb_pending`/ready metrics. |
| 7 | overload/backpressure rehearsal | `build/engine-readiness/20260314-0223-overload-rehearsal/` | fail | Initial rehearsal was later superseded: concurrent loadgen runs reused the same login IDs and produced an invalid duplicate-login collapse. |

## Phase 6 Remediation Checkpoints

| Order | Checkpoint | Run Root | Verdict | Key Outcome |
| --- | --- | --- | --- | --- |
| 8 | Redis worker degraded-state remediation | `build/engine-readiness/20260314-024138-redis-remediation/` | pass | `wb_worker` now drops to `503 not ready: deps=redis` during outage, flips `runtime_dependency_ready{name="redis",required="true"}` to `0`, and recovers automatically after Redis restart. |
| 9 | overload/backpressure remediation rerun | `build/engine-readiness/20260314-025202-overload-remediation-v3/` | pass | Identity-safe `8 x steady_chat` completed with `192` connected / authenticated / joined sessions, `0` login failures, and `0` total errors; both servers handled live traffic. |

## Deferred Caveats

- Gateway restart currently causes bounded session loss rather than transparent continuity for in-flight sessions behind the restarted instance.
- Server restart currently causes bounded backend-session loss rather than transparent continuity for in-flight sessions routed through the restarted instance.
- Worker restart recovery is visible, but backlog depth signaling is still weaker than ideal because `wb_pending` did not spike in the sampled window.

## Accepted Baseline Output

- `ready to branch`: yes
- `reason`: the remaining shared-runtime caveats are bounded restart/backlog-visibility concerns, not baseline blockers; the previous overload blocker was closed by fixing concurrent loadgen identity collisions and tightening gateway equal-load backend selection.
- detailed Phase 4 decision: `docs/ops/engine-readiness-decision.md`
- Phase 5 branch-cut criteria: `docs/ops/engine-branch-cut-criteria.md`
- historical preferred first genre branch after the capability-first tranche: `engine-roadmap-mmorpg`

## Downstream Follow-Up Ownership

- transparent in-flight continuity caveats belong to downstream session-continuity / MMORPG work, not to this closed baseline ledger
- gameplay-grade UDP/RUDP follow-up belongs to downstream FPS work, not to this closed baseline ledger
- no further checkpoints are planned here unless a later branch exposes a new shared blocker that invalidates the accepted baseline
