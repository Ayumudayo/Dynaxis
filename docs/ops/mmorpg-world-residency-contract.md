# MMORPG World Residency Contract

This document records the current world admission and residency contract on `engine-roadmap-mmorpg`.

## Scope

- This slice adds durable world residency on top of the continuity substrate.
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
- the residency key is persisted separately from room continuity
- room continuity is now subordinate to world continuity:
  - if world residency is restored, the room may be restored
  - if world residency is missing, room restore is not trusted and the session falls back to `lobby`

### Storage

- world residency lives at a dedicated continuity Redis key
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
  - room continuity

### Resume login

- if the persisted world key exists:
  - restore `world_id`
  - attempt room restore from the room continuity key
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

## Proof Targets

- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
