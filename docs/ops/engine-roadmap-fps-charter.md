# Engine Roadmap FPS Charter

This document is the branch-start charter for `engine-roadmap-fps`.

It is intentionally narrow. The first value is gameplay-frequency transport/runtime proof beyond attach-only UDP/RUDP visibility, not a broad shooter feature set.

## Branch Entry

- branch: `engine-roadmap-fps`
- source baseline decision: `docs/ops/engine-readiness-decision.md`
- branch-cut criteria reference: `docs/ops/engine-branch-cut-criteria.md`

## Canonical References

- `docs/ops/engine-fps-fixed-step-boundary.md`
- `docs/protocol/rudp.md`
- `tools/loadgen/README.md`

## Why This Branch Exists

- the accepted common baseline deferred gameplay-grade UDP/RUDP maturity to FPS-oriented follow-up work
- current transport proof already covers UDP bind attach and RUDP HELLO success/fallback/OFF behavior
- gameplay-frequency direct data for repeated frames still needs proof before replication systems should start

## First-Tranche Scope

### 1. Fixed-step runtime boundary

- define the minimum engine-owned fixed-step/runtime boundary that later FPS work will rely on
- keep the contract explicit between network ingress, scheduler-driven periodic work, and a future authoritative simulation tick
- stop at the ownership note; do not start a gameplay loop

### 2. Minimal direct data path

- use `MSG_PING` as the first narrow gameplay-frequency proof frame
- widen direct-path eligibility beyond attach-only traffic
- keep the proof on the same-gateway TCP + UDP path rather than HAProxy

### 3. Proof harness

- extend `stack_loadgen` so `udp` and `rudp` transports support direct `ping`
- retain deterministic proof scenarios and short mixed soaks for UDP/RUDP ping
- keep RUDP success, fallback, and OFF behavior observable with the existing rollout env toggles

## Explicit Non-Goals

- no snapshot/delta replication primitives
- no interest management
- no lag compensation
- no full authoritative simulation loop
- no MMORPG persistence/session/world lifecycle work
- no reopening shared baseline blocker analysis

## Verification Bar

- attach-only UDP/RUDP proofs stay green
- same-gateway UDP ping proof passes
- same-gateway RUDP ping proof passes
- RUDP fallback and OFF keep the ping workload bounded and observable
- loadgen reports, metrics snapshots, and short result summaries are retained
