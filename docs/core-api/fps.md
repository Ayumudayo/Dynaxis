# FPS Capability API Guide

## Stability

| Header | Stability |
|---|---|
| `server/core/fps/direct_bind.hpp` | `[Stable]` |
| `server/core/fps/direct_delivery.hpp` | `[Stable]` |
| `server/core/fps/transport_quality.hpp` | `[Stable]` |
| `server/core/fps/transport_policy.hpp` | `[Stable]` |
| `server/core/fps/runtime.hpp` | `[Stable]` |

## Scope
- This surface promotes the engine-neutral fixed-step FPS runtime substrate.
- It does not expose gateway transport internals, session implementation details, combat rules, or game-specific schemas.
- The public contract is the authoritative tick/runtime/replication model, not the app-local wire encoding.
- direct UDP/RUDP transport rollout is still broader than this guide, but bind payload contract is now public via `direct_bind.hpp`, rollout/canary/allowlist policy is public via `transport_policy.hpp`, and direct delta route selection policy is public via `direct_delivery.hpp`.

## Public Contract
- `DirectBindRequest`, `DirectBindTicket`, and `DirectBindResponse` define the bind payload contract for direct UDP attach.
- `encode_direct_bind_request_payload()` / `decode_direct_bind_request_payload()` and `encode_direct_bind_response_payload()` / `decode_direct_bind_response_payload()` define the stable payload encoding contract above the generic packet header.
- `parse_direct_opcode_allowlist()` parses decimal/hex CSV allowlists into a stable opcode set contract.
- `DirectTransportRolloutPolicy` defines:
  - `enabled`
  - `canary_percent`
  - `opcode_allowlist`
  - deterministic `session_selected()`
  - explicit `opcode_allowed()`
- `evaluate_direct_attach()` makes bind-time attach semantics explicit.
  - current attach outcomes cover:
    - rollout disabled -> UDP only
    - allowlist empty -> UDP only
    - canary miss -> UDP only
    - canary selected -> RUDP candidate
- `evaluate_direct_delivery()` is the engine-neutral egress policy seam for choosing `tcp fallback`, `udp`, or `rudp` once the app has decided a message is eligible for direct delivery.
  - it returns both `route` and an explicit `reason`
  - current reasons cover:
    - message not eligible
    - not UDP-bound
    - UDP direct
    - RUDP direct
    - RUDP fallback latched to TCP
    - RUDP handshake pending with temporary UDP direct delivery
- `UdpSequencedMetrics` exposes the direct UDP sequenced ingress quality contract.
  - accepted forward progress packets update estimated loss and jitter signals
  - stale packets are classified as duplicate or reordered without being accepted
  - `reset()` clears the replay window for a fresh bind/rebind lifecycle
- `FixedStepDriver` provides bounded catch-up authoritative tick accumulation.
- `WorldRuntime` owns:
  - tick-aligned input staging
  - authoritative actor movement/state advancement
  - coarse interest selection
  - snapshot/delta replication shaping
  - bounded rewind/history retention
- `RuntimeConfig` exposes:
  - `tick_rate_hz`
  - `snapshot_refresh_ticks`
  - `interest_cell_size_mm`
  - `interest_radius_cells`
  - `max_interest_recipients_per_tick`
  - `max_delta_actors_per_tick`
  - `history_ticks`

## Semantics
- `stage_input()` returns an explicit `StageInputResult`.
  - accepted inputs are staged for `target_server_tick = current_tick + 1`
  - stale inputs are rejected without mutating authoritative state
- `tick()` returns generic `ReplicationUpdate` entries.
  - `kSnapshot` is a full authoritative repair/update for that viewer
  - `kDelta` carries only dirty visible actors plus removals
  - when dirty visible actors exceed `max_delta_actors_per_tick`, the runtime falls back to `kSnapshot`
- `rewind_at_or_before()` is the public lag-compensation lookup seam.
  - it returns the latest authoritative sample at or before the requested tick
  - `exact_tick=false` means the result is a fallback sample, not an exact-tick match

## Non-Goals
- no UDP/RUDP handshake or transport rollout logic
- no wire/protobuf/game-opcode encoding contract
- no weapon/combat/product logic
- no migration/session continuity behavior

## Public Proof
- contract proof target: `CorePublicApiFpsCapabilitySmoke`
- installed consumer proof: `CoreInstalledPackageConsumer`
- preferred Phase 2 acceptance proof:
  - `CorePublicApiFpsCapabilitySmoke`
  - `tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`
- lower-level stack transport proof:
  - `tests/python/verify_fps_rudp_transport_matrix.py`
  - current matrix includes direct RUDP attach/fallback/restart plus deterministic direct-UDP packet-quality impairment proof
