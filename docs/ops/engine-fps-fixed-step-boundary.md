# FPS Fixed-Step Runtime Boundary

This note fixes the minimum ownership boundary for the first `engine-roadmap-fps` tranche.

The goal is not to introduce a gameplay loop yet. The goal is to stop later FPS work from rediscovering where transport, scheduler, and simulation responsibilities separate.

## Boundary

### 1. Network ingress remains transport-facing

- `gateway_app` owns direct UDP/RUDP ingress, attach/fallback policy, and transport observability.
- `server_app` continues to receive validated application frames through the existing dispatcher path.
- The first FPS slice only widens ingress eligibility for `MSG_PING`; it does not add replication or state-delta semantics.

### 2. Scheduler-driven periodic work remains service/runtime-facing

- existing timers, watchdogs, and maintenance work stay in the scheduler/control plane layer
- periodic runtime work may prepare future tick inputs, but it does not mutate authoritative gameplay state on its own
- any cadence used for proof or health traffic is still a runtime concern, not a simulation contract

### 3. Future authoritative tick is a separate engine-owned boundary

- the future authoritative simulation tick will consume validated inputs after transport ingress and dispatch
- tick cadence, state advancement, reconciliation, and replication fanout belong to that future engine-owned layer
- this tranche explicitly avoids implementing ECS, combat, replication snapshots, lag compensation, or interest management

## Ownership Rule

For FPS follow-up work, ask one question first:

`Is this change about getting a frame safely into the runtime, or about advancing authoritative gameplay state?`

- if it is about transport admission, fallback, framing, or direct-path proof, it belongs below the boundary
- if it is about advancing world state on fixed cadence, it belongs above the boundary

The first FPS slice only hardens the lower boundary and documents the seam. It does not cross into the authoritative tick layer.
