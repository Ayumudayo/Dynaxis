# Engine Readiness Baseline Checkpoints

This document is the tracked checkpoint ledger for the `engine-readiness-baseline` branch.

The detailed working runbooks and raw artifact notes live under local-only `tasks/` and ignored `build/engine-readiness/`.
This file exists so each executed checkpoint can leave a commit-visible summary.

## Current Rule

- Execute checkpoints strictly in runbook order.
- Keep each completed checkpoint small enough to be committed independently.
- Use the run root under `build/engine-readiness/<run_id>/` as the raw evidence bundle for the matching checkpoint summary here.

## Phase 3 Checkpoints

| Order | Checkpoint | Run Root | Verdict | Key Outcome |
| --- | --- | --- | --- | --- |
| 1 | Control baseline rerun | `build/engine-readiness/20260312-0228-control-baseline/` | pass | TCP/UDP/RUDP control comparator stayed clean enough to begin failure/recovery rehearsals. |
| 2 | Redis outage/recovery | `build/engine-readiness/20260314-0150-redis-recovery/` | pass with caveat | gateway/server degraded visibly and recovered automatically; worker Redis outage signaling stayed weaker than gateway/server signaling. |
| 3 | Postgres outage/recovery | `build/engine-readiness/20260314-0155-postgres-recovery/` | pass | server/worker advertised DB degradation cleanly, worker retry/reconnect signals were explicit, and recovery completed automatically. |
| 4 | gateway restart during live traffic | `build/engine-readiness/20260314-0212-gateway-restart/` | pass with caveat | stack recovered and new traffic stayed routable, but the live soak recorded `24` disconnects / `24` errors for sessions on the restarted gateway. |
| 5 | server restart during live traffic | `build/engine-readiness/20260314-0216-server-restart/` | pass with caveat | stack recovered and probes kept passing, but the live soak recorded `26` disconnects / `26` errors for sessions routed to the restarted backend. |

## Active Caveats

- Redis outage signaling from `wb_worker` is weaker than the gateway/server story in the sampled window.
- Gateway restart currently causes bounded session loss rather than transparent continuity for in-flight sessions behind the restarted instance.
- Server restart currently causes bounded backend-session loss rather than transparent continuity for in-flight sessions routed through the restarted instance.

## Next Checkpoint

- `worker restart during backlog processing`

After that:

- `overload/backpressure rehearsal`
