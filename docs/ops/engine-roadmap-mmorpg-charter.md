# Engine Roadmap MMORPG Charter

This document is the branch-start charter for `engine-roadmap-mmorpg`.

It is intentionally cut only after the first `engine-roadmap-session-continuity` tranche closed and merged, so this branch can start from continuity primitives instead of rebuilding them.

## Branch Cut

- branch: `engine-roadmap-mmorpg`
- cut from: merged `engine-roadmap-session-continuity`
- cut commit: `db380b5`
- continuity tranche closure: `docs/ops/engine-roadmap-session-continuity-first-tranche-closure.md`
- branch-cut criteria reference: `docs/ops/engine-branch-cut-criteria.md`

## Why This Branch Exists

- The common baseline is already accepted.
- The continuity branch now provides:
  - resumable logical session identity
  - restart-aware reconnect proof
  - shard-aware locator fallback
  - observable continuity handoff metrics
- The next gap is no longer generic reconnect logic; it is MMORPG-style world/session orchestration on top of that substrate.

## First-Tranche Theme

- world admission and residency on top of session continuity

This branch still starts with backend/control-plane work, not gameplay simulation.

## First-Tranche Scope

### 1. World admission contract

- define the minimum metadata required to admit a logical session into a world/shard boundary
- make world admission target selection explicit instead of inferring it from the current chat room model
- preserve a safe fallback path when the previous world target is unavailable

### 2. Durable residency state

- separate lightweight world residency from ephemeral transport attachment
- define where world residency lives and which component owns writes
- ensure reconnect can recover:
  - logical session identity
  - current world/shard residency
  - safe lobby or equivalent fallback when residency cannot be restored

### 3. Background ownership boundary

- define which runtime owns shard/world-scoped background work
- make ownership visible enough to reason about restart handoff
- keep this at the ownership/contract level before introducing broad orchestration machinery

### 4. Proof target

- add a proof that world admission and residency restore deterministically after bounded restart/reconnect events
- retain evidence comparable to the continuity tranche result bundles

## Explicit Non-Goals

- no FPS gameplay replication or lag compensation work
- no gameplay-frequency UDP/RUDP expansion
- no live zone migration choreography
- no combat/system simulation layer
- no broad multi-service world architecture rewrite before the admission/residency proof is green

## Verification Bar

This first MMORPG tranche is only done when all items below are satisfied.

### 1. Admission/residency contract is explicit

- the world admission target model is written down
- ownership of residency state is unambiguous
- restart/reconnect decision rules are explicit

### 2. Recovery proof exists

- reconnect can restore the same world/shard residency when it is still valid
- reconnect falls back deterministically when the previous residency target is unavailable
- failure/recovery remains observable in metrics/logs

### 3. Evidence artifacts are retained

- targeted proof outputs
- readiness/metrics snapshots
- relevant service log tails
- short result summaries under retained run roots

## First Implementation Slice

1. define world admission metadata on top of the existing continuity locator
2. define durable world residency ownership and fallback rules
3. expose residency handoff metrics
4. prove bounded restart/reconnect recovery for world admission
