# Engine Readiness Baseline Checkpoints

This document records the accepted common-baseline evidence that was established before the downstream continuity/MMORPG/FPS tranches merged.
The baseline itself is closed; this file remains as the retained checkpoint ledger.

## Baseline Status

- `accepted shared-proof baseline`: yes
- `branch split`: completed historically
- `downstream work on top of the baseline`: merged into `main`
- remaining restart/backlog caveats are follow-up quality concerns, not baseline blockers

## Historical Execution Rule

- checkpoints were executed in bounded, reviewable slices
- raw evidence lived under `build/engine-readiness/<run_id>/`
- detailed working notes were kept in local-only `tasks/`

## Checkpoint Ledger

| Order | Checkpoint | Run Root | Verdict | Key Outcome |
| --- | --- | --- | --- | --- |
| 1 | Control baseline rerun | `build/engine-readiness/20260312-0228-control-baseline/` | pass | TCP/UDP/RUDP control comparator stayed clean enough to begin failure/recovery rehearsals. |
| 2 | Redis outage/recovery | `build/engine-readiness/20260314-0150-redis-recovery/` | pass with caveat | gateway/server degraded visibly and recovered automatically; worker Redis outage signaling stayed weaker than gateway/server signaling. |
| 3 | Postgres outage/recovery | `build/engine-readiness/20260314-0155-postgres-recovery/` | pass | server/worker advertised DB degradation cleanly and recovered automatically. |
| 4 | gateway restart during live traffic | `build/engine-readiness/20260314-0212-gateway-restart/` | pass with caveat | stack recovered and new traffic stayed routable, but sessions on the restarted gateway were lost. |
| 5 | server restart during live traffic | `build/engine-readiness/20260314-0216-server-restart/` | pass with caveat | stack recovered and probes kept passing, but sessions on the restarted backend were lost. |
| 6 | worker restart during backlog processing | `build/engine-readiness/20260314-0219-worker-restart/` | pass with caveat | backlog drained after restart, though visibility was stronger in flush logs than in backlog-depth metrics. |
| 7 | overload/backpressure rehearsal | `build/engine-readiness/20260314-0223-overload-rehearsal/` | superseded | initial run was invalidated by duplicate-login collisions across concurrent loadgen processes. |
| 8 | Redis worker degraded-state remediation | `build/engine-readiness/20260314-024138-redis-remediation/` | pass | worker Redis dependency signaling became explicit and aligned with gateway/server behavior. |
| 9 | overload/backpressure remediation rerun | `build/engine-readiness/20260314-025202-overload-remediation-v3/` | pass | identity-safe rerun completed with `0` login failures and `0` total errors. |

## Accepted Conclusion

- the shared baseline was accepted on 2026-03-14
- later downstream work was allowed to proceed without reopening common-baseline blockers
- the following downstream lines have since merged:
  - session continuity substrate
  - MMORPG world residency/lifecycle substrate
  - FPS transport/runtime substrate

## Deferred Caveats

- transparent in-flight continuity across gateway/server restart remained deferred beyond the baseline bar
- worker backlog-depth visibility remained weaker than ideal
- gameplay-grade UDP/RUDP maturity remained a downstream FPS concern rather than a common-baseline blocker

## Related Docs

- `docs/ops/engine-readiness-decision.md`
- `docs/ops/session-continuity-contract.md`
- `docs/ops/mmorpg-world-residency-contract.md`
- `docs/ops/realtime-runtime-contract.md`
