# Engine Roadmap MMORPG World Lifecycle Charter

This document is the next tranche charter for `engine-roadmap-mmorpg` after the first admission/residency tranche closed.

## Start Point

- previous tranche closure: `docs/ops/engine-roadmap-mmorpg-first-tranche-closure.md`
- previous merged PR: `Ayumudayo/Dynaxis#15`
- continuity substrate reference: `docs/ops/session-continuity-contract.md`
- world admission/residency reference: `docs/ops/mmorpg-world-residency-contract.md`

## Why This Tranche Exists

The runtime now knows how to restore a world-bound logical session safely.

The next gap is not "can the stack remember a world id?" but:

- can operators see the world boundary clearly
- can lifecycle ownership be reasoned about from the control plane
- can later world drain/reassignment work build on explicit visible contracts instead of hidden Redis keys

## Tranche Theme

- world lifecycle visibility and control-plane policy on top of the first world residency boundary

## Scope

### 1. Control-plane world visibility

- expose world-bound instance state in admin/control-plane payloads
- make owner-match vs owner-mismatch visible without Redis shell access
- keep the contract small and operationally legible

### 2. Lifecycle policy boundary

- define which lifecycle questions the control plane can answer now
- separate "visible owner boundary" from future "active handoff/orchestration"
- keep policy explicit before adding world drain or reassignment machinery

### 3. Proof target

- retain bounded admin/control-plane proof that world scope is visible and consistent with the continuity owner boundary
- keep runtime restart/fallback proof green while adding the new surface

## Explicit Non-Goals

- no live world migration
- no automatic owner transfer choreography
- no gameplay simulation or combat systems
- no broad multi-service world scheduler in this tranche

## Verification Bar

- `admin_app` exposes world scope for world-tagged server instances
- world scope includes owner visibility sufficient to explain restore decisions
- existing continuity/world fallback rehearsals still pass
- admin/control-plane smoke proves the new fields end to end

## First Slice

1. expose world scope and owner-match data in `admin_app`
2. prove the new surface with `verify_admin_api.py`
3. retain the existing restart/fallback runtime proofs
