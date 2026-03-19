# Realtime Runtime Contract

This document records the current realtime-oriented transport/runtime substrate now merged into `main`.
Today the substrate is exercised primarily through the FPS input workload, but the public engine contract is the canonical `server/core/realtime/**` surface rather than the older `server/core/fps/**` wrapper path.

## Canonical Surface

- canonical public headers:
  - `server/core/realtime/direct_bind.hpp`
  - `server/core/realtime/direct_delivery.hpp`
  - `server/core/realtime/transport_quality.hpp`
  - `server/core/realtime/transport_policy.hpp`
  - `server/core/realtime/runtime.hpp`
- canonical namespace: `server::core::realtime`
- `server/core/fps/**` remains a 2.x compatibility wrapper only; migration/removal plan is tracked in `docs/core-api/fps-to-realtime-migration.md`

## Current Scope

- direct UDP/RUDP proof moved beyond attach-only visibility and now covers direct `MSG_PING` plus direct `MSG_FPS_INPUT` ingress
- the server contains a public `server/core/realtime/runtime.hpp` fixed-step runtime for authoritative 2D actor transforms
- replication currently uses reliable TCP snapshots plus gameplay-frequency delta delivery
- coarse interest filtering and actor history retention are implemented as engine substrate, not game-rule logic

## Ownership Boundary

### Transport ingress

- `gateway_app` accepts direct UDP/RUDP ingress only for the approved proof/gameplay substrate messages
- `MSG_PING` and `MSG_FPS_INPUT` may enter over direct UDP/RUDP when policy/env allow it
- unsupported workloads remain TCP-only and should fail in scenario validation rather than degrade silently

### Fixed-step runtime

- the realtime runtime uses its own fixed-step timer/accumulator; it does not repurpose `TaskScheduler` as an authoritative tick
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
  - rollout-disabled / canary-miss sessions stay on direct UDP once UDP bind succeeded
  - unbound sessions and RUDP fallback-latched sessions fall back to the existing TCP bridge path
  - RUDP-selected but not-yet-established sessions temporarily use UDP direct delivery until the handshake settles or fallback latches

## Interest And History Rules

- interest management is coarse cell-based selection, not fine-grained relevance logic
- visibility changes queue a fresh snapshot instead of inventing a more complex partial-repair protocol
- delta actor fanout is budgeted; when dirty visible actors exceed the configured delta budget, the runtime falls back to snapshot repair for that viewer
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
  - `CorePublicApiRealtimeCapabilitySmoke`
  - `CoreFpsCompatSmoke`
  - `tests/python/verify_fps_state_transport.py`
  - `tests/python/verify_fps_rudp_transport.py --scenario attach`
  - `tests/python/verify_fps_rudp_transport.py --scenario off`
  - `tests/python/verify_fps_rudp_transport.py --scenario rollout-fallback`
  - `tests/python/verify_fps_rudp_transport.py --scenario protocol-fallback`
  - `tests/python/verify_fps_rudp_transport.py --scenario udp-quality-impairment`
  - `tests/python/verify_fps_rudp_transport.py --scenario restart`
  - `tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`
  - `tests/python/verify_fps_rudp_transport_matrix.py`
  - deterministic impaired direct-UDP proof now exercises sequence gap, duplicate, reorder, and jitter signals through `gateway_udp_loss_estimated_total`, `gateway_udp_jitter_ms_last`, `gateway_udp_reorder_drop_total`, and `gateway_udp_duplicate_drop_total`
  - `tests/server/test_fps_runtime.cpp`
  - `tests/core/test_direct_egress_route.cpp`
  - `tests/core/test_rudp_engine.cpp`

## Phase 2 Acceptance Boundary

- current Phase 2 acceptance evidence is:
  - public-consumer realtime capability proof through `CorePublicApiRealtimeCapabilitySmoke`
  - transitional fps wrapper proof through `CoreFpsCompatSmoke`
  - installed-consumer runtime proof through `CoreInstalledPackageConsumer`
  - direct UDP/RUDP fallback/restart/impaired-network proof through `tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`
- the remaining larger transport gap is no longer Phase 2 substrate proof; it is richer quantified/network-shaping evidence such as fuller OS-level netem rehearsal, which belongs to later release-evidence work
- the current OS-level netem path is a manual ops-only rehearsal through `python tests/python/verify_fps_netem_rehearsal.py --scenario fps-pair`; it is intentionally not part of the accepted baseline or `ci-hardening`
- the Phase 5 quantitative baseline, including preferred runner/log path and provisional transport budgets, is recorded in `docs/ops/quantified-release-evidence.md`
- the first captured FPS direct-path soak reports are:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-010307Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-010307Z.host.json`

## Explicit Non-Goals

- no weapon/fire/combat rules
- no lag-compensated hit validation
- no broad snapshot/delta protocol family beyond the current substrate
- no gameplay-state continuity or world-migration semantics
