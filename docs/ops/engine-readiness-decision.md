# Engine Readiness Baseline Decision

This document records the accepted decision that the common engine-readiness baseline was sufficient to unblock downstream runtime work.
The decision remains historical but still explains why the baseline was allowed to close.

## Decision Date

- 2026-03-14

## Decision

- `ready to split from the common baseline`: yes
- `reason`: outage/recovery and restart rehearsals were bounded and recoverable, the worker Redis signaling gap was closed, and the overload blocker was invalidated then rerun cleanly
- `current state`: the downstream continuity, MMORPG, and FPS tranches that depended on this decision have since merged into `main`

## Resolved Common Blockers

### Worker Redis degraded-state visibility

- `wb_worker` now advertises Redis degradation through readiness and dependency metrics
- the earlier worker-side observability gap is considered closed for branch-cut purposes

### Overload/login-collapse blocker

- the original failing overload rehearsal reused the same login IDs across concurrent loadgen processes
- that result was invalid as an engine overload signal
- the accepted rerun used identity-safe usernames and completed with `0` login failures and `0` total errors

## Deferred Caveats

- transparent in-flight continuity across gateway/server restart remained a downstream concern, not a common-baseline blocker
- worker backlog-depth visibility remained weaker than ideal but did not invalidate recovery correctness
- gameplay-grade UDP/RUDP transport maturity remained explicitly outside the common baseline

## Current Downstream Ownership

- continuity and restart semantics: `docs/ops/session-continuity-contract.md`
- world residency/lifecycle and operator policy: `docs/ops/mmorpg-world-residency-contract.md`
- gameplay-frequency transport/runtime substrate: `docs/ops/fps-runtime-contract.md`

This document is not a live branch tracker anymore; it is the retained explanation for why the shared baseline was allowed to close.
