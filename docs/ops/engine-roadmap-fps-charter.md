# Engine Roadmap FPS Charter

This document is the prepared branch-start charter for `engine-roadmap-fps`.

It is intentionally prepared after the common baseline was accepted and after the continuity/MMORPG-oriented backend tracks proved that the current stack is stronger in control-plane recovery than in gameplay-grade realtime transport.

## Branch Entry

- branch: `engine-roadmap-fps`
- branch status today: prepared, not cut yet
- source baseline decision: `docs/ops/engine-readiness-decision.md`
- branch-cut criteria reference: `docs/ops/engine-branch-cut-criteria.md`

## Why This Branch Exists

- The accepted common baseline explicitly deferred gameplay-grade UDP/RUDP transport maturity to FPS-oriented follow-up work.
- The current transport proof is still attach-oriented:
  - UDP bind attach is proven
  - RUDP HELLO success/fallback/OFF semantics are proven
  - gameplay-frequency direct data over UDP/RUDP is **not** yet proven
- The next FPS value is therefore transport/runtime hardening beyond attach-only visibility, not snapshot/delta replication or full simulation systems.

## First-Tranche Theme

- gameplay-frequency transport substrate before replication features

This branch starts with a narrow transport/runtime slice.
It does not claim that the engine already has authoritative shooter simulation semantics.

## First-Tranche Scope

### 1. Fixed-step runtime boundary

- define the minimum engine-owned fixed-step/runtime-loop boundary that future FPS work will build on
- make the contract explicit between:
  - network I/O and gateway/server event handling
  - scheduler-driven periodic work
  - a future authoritative simulation tick
- keep this as a boundary/contract decision in the first tranche; do not start full ECS or gameplay-state execution yet

### 2. Minimal gameplay-frequency direct data path

- widen the current transport proof beyond attach-only traffic
- use existing `MSG_PING` as the first minimal cross-transport proof frame
- make `MSG_PING` valid for the chosen direct UDP/RUDP proof path so the stack can exercise repeated gameplay-like traffic without inventing replication semantics in the same slice
- keep the proof on the direct same-gateway TCP+UDP path, not HAProxy

### 3. Gameplay-rate proof harness

- extend `stack_loadgen` so `udp` and `rudp` transports support `ping` mode after bind/attach
- add deterministic probe scenarios:
  - `tools/loadgen/scenarios/udp_ping_only.json`
  - `tools/loadgen/scenarios/rudp_ping_only.json`
- add gameplay-rate soak scenarios:
  - `tools/loadgen/scenarios/mixed_direct_udp_ping_soak.json`
  - `tools/loadgen/scenarios/mixed_direct_rudp_ping_soak.json`
- reuse the existing fallback/OFF env toggles to prove:
  - RUDP success path
  - RUDP forced fallback
  - RUDP OFF invariance

### 4. Transport observability for the widened path

- keep the first tranche explainable through existing transport observability wherever possible:
  - `core_runtime_rudp_*`
  - `gateway_rudp_*`
  - loadgen transport breakdown and latency/error counters
- add new metrics only if the widened ping path is not explainable from the current surface

## Explicit Non-Goals

- no snapshot/delta replication primitives
- no interest-management system
- no lag-compensation hooks
- no full authoritative gameplay/simulation loop implementation
- no MMORPG persistence/session/world lifecycle work
- no reopening already-accepted shared baseline blocker analysis as primary work

## Verification Bar

This first FPS tranche is only done when all items below are satisfied.

### 1. Fixed-step boundary is explicit

- the first FPS runtime boundary is written down clearly enough that later replication/tick work does not need to rediscover ownership
- the branch still avoids promising a full gameplay simulation model

### 2. Direct gameplay-rate transport proof exists

- same-gateway UDP ping proof passes after TCP bootstrap + UDP bind
- same-gateway RUDP ping proof passes after TCP bootstrap + UDP bind + RUDP attach
- RUDP fallback and OFF runs keep the ping workload bounded and observable rather than failing silently

### 3. Existing transport regressions stay green

- attach-only UDP proof still passes
- attach-only RUDP success/fallback/OFF proof still passes
- accepted common baseline docs do not need to be reopened to explain the new FPS slice

### 4. Evidence artifacts are retained

- targeted loadgen reports for `udp_ping_only` / `rudp_ping_only`
- retained mixed soak reports for UDP and RUDP ping scenarios
- pre/during/post metrics snapshots for the direct same-gateway runtime path
- relevant service log tails and short result summaries

## First Implementation Slice

Start with the narrowest slice that proves value:

1. define the fixed-step runtime boundary contract
2. promote `MSG_PING` from TCP-only to the minimal direct UDP/RUDP-capable proof frame
3. extend `stack_loadgen` with UDP/RUDP `ping` mode and the four branch-local scenarios
4. prove same-gateway UDP success, RUDP success, RUDP fallback, and RUDP OFF invariance for the ping path

Do not start snapshot/delta replication, interest management, or lag compensation before this slice is green.
