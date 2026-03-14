# MMORPG World Residency And Ownership Contract

This document records the current world admission, residency, and owner-boundary contract on `engine-roadmap-mmorpg`.

## Scope

- This slice adds durable world residency plus a world-scoped owner boundary on top of the continuity substrate.
- It still does not implement:
  - live zone migration
  - gameplay simulation state continuity
  - combat/world replication

## Ownership

### Gateway

- gateway resume locator hints now include `world_id`
- `world_id` is derived from the backend registry tag convention `world:<id>`
- if the exact resume alias binding is missing, selector fallback can still constrain reconnect toward the same world/shard boundary

### Server

- server owns durable world residency state for a logical session
- server also owns the current world-residency authority for its advertised world via `SERVER_INSTANCE_ID`
- the residency key is persisted separately from room continuity
- the owner key is persisted separately from both residency and room continuity
- room continuity is now subordinate to world continuity:
  - if world residency is restored, the room may be restored
  - if world residency is missing, room restore is not trusted and the session falls back to `lobby`
- room continuity is also subordinate to owner continuity:
  - if the persisted world owner matches the current backend owner, room restore may proceed
  - if the world owner is missing or mismatched, the session falls back to `default world + lobby`

### Storage

- world residency lives at a dedicated continuity Redis key
- world owner authority lives at a dedicated continuity Redis key keyed by `world_id`
- room continuity still lives at its own continuity Redis key
- both keys share the lease-shaped TTL window

## Decision Rules

### Fresh login

- assign `world_id` from:
  - `WORLD_ADMISSION_DEFAULT`, if set
  - otherwise the first `world:<id>` tag from `SERVER_TAGS`
  - otherwise `default`
- persist both:
  - world residency
  - world owner authority
  - room continuity

### Resume login

- if the persisted world key exists:
  - and the persisted world owner matches the current backend owner:
    - restore `world_id`
    - attempt room restore from the room continuity key
- if the persisted world key exists but the owner key is missing or mismatched:
  - fall back to the safe default world for the current backend
  - force room to `lobby`
- if the persisted world key is missing:
  - fall back to the safe default world for the current backend
  - force room to `lobby`

## Observable Signals

- login response now includes `world_id`
- server metrics now expose:
  - `chat_continuity_world_write_total`
  - `chat_continuity_world_write_fail_total`
  - `chat_continuity_world_restore_total`
  - `chat_continuity_world_restore_fallback_total`
  - `chat_continuity_world_owner_write_total`
  - `chat_continuity_world_owner_write_fail_total`
  - `chat_continuity_world_owner_restore_total`
  - `chat_continuity_world_owner_restore_fallback_total`

## Proof Targets

- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
