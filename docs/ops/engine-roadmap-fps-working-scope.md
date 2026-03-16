# Engine Roadmap FPS Working Scope

This document defines the full execution area for `engine-roadmap-fps`.

The branch is **not** PR-ready after the first ping-path slice alone.
The first slice only proved that direct gameplay-rate transport can move beyond attach-only UDP/RUDP visibility.

The rest of this branch must stay focused on FPS-oriented engine substrate, not on game-specific shooter features.

## Current Status

Completed in this branch:

- fixed-step/runtime ownership note:
  - `docs/ops/engine-fps-fixed-step-boundary.md`
- direct same-gateway UDP `MSG_PING` proof
- direct same-gateway RUDP `MSG_PING` proof
- RUDP fallback/OFF invariance on the ping path
- transport-aware loadgen scenarios and retained proof artifacts

Not yet complete:

- executable fixed-step tick scaffold
- snapshot/delta replication primitives
- interest-management primitives
- latency-compensation hooks
- an integrated FPS-substrate proof that exercises those layers together

## Branch Working Area

This branch owns the FPS-specific engine substrate below full gameplay rules.

### 1. Gameplay-rate transport/runtime hardening

- retain the direct UDP/RUDP ping-path proof as the baseline
- extend transport/runtime behavior toward reusable gameplay-rate frame handling
- keep rollout, fallback, and observability explainable under the existing gateway/core metrics surface unless a real gap appears

### 2. Executable fixed-step runtime scaffold

- turn the documented boundary into a minimal executable tick/runtime scaffold
- own tick cadence, input staging, and deterministic scheduling boundaries
- stop before full simulation content, combat rules, or a large ECS refactor

### 3. Snapshot/delta replication primitives

- add the minimum reusable snapshot/delta contracts needed for authoritative state replication
- keep the first implementation narrow:
  - one minimal replicated state shape
  - deterministic encode/decode/update behavior
- do not mix gameplay-specific abilities, weapons, or content rules into these primitives

### 4. Interest-management primitives

- add coarse receiver-selection primitives that later FPS state fanout can reuse
- keep the first pass structural:
  - recipient filtering
  - bounded selection policy
  - proof/metrics for fanout behavior
- do not jump to full world-streaming or MMORPG zone/shard ownership work

### 5. Latency-compensation hooks

- add the interfaces and timing/history hooks needed for later lag-compensation work
- keep them as engine hooks and retained state contracts
- do not implement full hit validation, rewind-based combat resolution, or anti-cheat policy here

### 6. Proof harness and acceptance evidence

- extend loadgen/tests/metrics so the branch can prove the substrate end to end
- retain short deterministic proofs and mixed soak evidence for each newly added layer
- keep branch-local evidence under `build/engine-roadmap-fps/`

## Explicitly Out Of Scope

These items do **not** belong in `engine-roadmap-fps` before PR:

- shooter gameplay rules:
  - weapons
  - damage
  - hit logic
  - abilities
- matchmaking or game-mode logic
- content/data authoring
- MMORPG persistence, world lifecycle, or session continuity expansion
- admin control-plane/topology work unless directly required to prove FPS substrate behavior
- broad repo cleanup unrelated to FPS substrate delivery

## Next Execution Order

The intended order for the remaining branch work is:

1. implement the executable fixed-step tick scaffold
2. add one minimal snapshot/delta replication path on top of that scaffold
3. add coarse interest-management primitives for that replicated path
4. add latency-compensation hooks/history surfaces without full gameplay resolution
5. rerun integrated proof, soak, and failure-mode validation across transport + tick + replication

## Done Definition Before PR

`engine-roadmap-fps` is ready for PR only when all of the following are true:

- the direct UDP/RUDP ping-path proof remains green
- the branch contains an executable fixed-step runtime scaffold, not just a note
- at least one minimal snapshot/delta replication primitive is implemented and validated
- coarse interest-management primitives are implemented and validated
- latency-compensation hooks exist at the engine-contract level
- integrated branch evidence is retained and summarized
- the branch still avoids MMORPG scope and full game-rule implementation
