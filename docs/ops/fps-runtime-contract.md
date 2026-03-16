# FPS Runtime Contract

This document records the current FPS-oriented transport/runtime substrate that is now merged into `main`.
It defines the live ownership boundary and the intentionally narrow proof scope for gameplay-frequency transport and replication.

## Current Scope

- direct UDP/RUDP proof moved beyond attach-only visibility and now covers direct `MSG_PING` plus direct `MSG_FPS_INPUT` ingress
- the server contains a neutral fixed-step runtime for authoritative 2D actor transforms
- replication currently uses reliable TCP snapshots plus gameplay-frequency delta delivery
- coarse interest filtering and actor history retention are implemented as engine substrate, not game-rule logic

## Ownership Boundary

### Transport ingress

- `gateway_app` accepts direct UDP/RUDP ingress only for the approved proof/gameplay substrate messages
- `MSG_PING` and `MSG_FPS_INPUT` may enter over direct UDP/RUDP when policy/env allow it
- unsupported workloads remain TCP-only and should fail in scenario validation rather than degrade silently

### Fixed-step runtime

- the FPS runtime uses its own fixed-step timer/accumulator; it does not repurpose `TaskScheduler` as an authoritative tick
- the runtime owns:
  - latest per-session staged input
  - actor creation on first FPS input
  - authoritative actor transform advancement
  - coarse interest selection
  - per-actor history retention

### Replication boundary

- authoritative state is intentionally minimal:
  - `actor_id`
  - `x_mm`
  - `y_mm`
  - `yaw_mdeg`
  - `last_applied_input_seq`
  - `server_tick`
- reliable resync stays on TCP through `MSG_FPS_STATE_SNAPSHOT`
- gameplay-frequency state update delivery uses `MSG_FPS_STATE_DELTA`
  - UDP-bound sessions receive direct UDP delta
  - established RUDP sessions receive direct RUDP delta
  - unbound/fallback/OFF sessions fall back to the existing TCP bridge path

## Interest And History Rules

- interest management is coarse cell-based selection, not fine-grained relevance logic
- visibility changes queue a fresh snapshot instead of inventing a more complex partial-repair protocol
- actor history is retained as a bounded per-actor ring buffer for "latest sample at or before tick" lookup
- lag compensation, rewind hit validation, combat, and shooter gameplay rules remain out of scope

## Current Proof Surface

- deterministic transport proofs:
  - `tools/loadgen/scenarios/udp_ping_only.json`
  - `tools/loadgen/scenarios/rudp_ping_only.json`
  - `tools/loadgen/scenarios/udp_fps_input_only.json`
  - `tools/loadgen/scenarios/rudp_fps_input_only.json`
- mixed soak proofs:
  - `tools/loadgen/scenarios/mixed_direct_udp_ping_soak.json`
  - `tools/loadgen/scenarios/mixed_direct_rudp_ping_soak.json`
  - `tools/loadgen/scenarios/mixed_direct_udp_fps_soak.json`
  - `tools/loadgen/scenarios/mixed_direct_rudp_fps_soak.json`
- stack/runtime verification:
  - `tests/python/verify_fps_state_transport.py`
  - `tests/server/test_fps_runtime.cpp`
  - `tests/core/test_direct_egress_route.cpp`
  - `tests/core/test_rudp_engine.cpp`

## Explicit Non-Goals

- no weapon/fire/combat rules
- no lag-compensated hit validation
- no broad snapshot/delta protocol family beyond the current substrate
- no gameplay-state continuity or world-migration semantics
