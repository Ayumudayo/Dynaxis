# Redis Strategy

This document records the current Redis usage model in Dynaxis.
Redis is not the source of record; it is the low-latency runtime layer around the PostgreSQL-backed system of record.

## Current Roles

- continuity/session cache
- presence and room membership cache
- Pub/Sub broadcast fanout
- recent-history cache
- write-behind stream transport
- rate-limit and idempotency keys

## Key Patterns

- session:
  - `chat:{env}:session:{token_hash}`
- presence:
  - `chat:{env}:presence:user:{user_id}`
  - `chat:{env}:presence:room:{room_id}`
- room membership:
  - `chat:{env}:room:{room_id}:members`
- recent history:
  - `chat:{env}:room:{room_id}:recent`
  - `chat:{env}:msg:{message_id}`
- rate/idempotency:
  - `chat:{env}:rate:{user_id}:{bucket}`
  - `chat:{env}:idem:{client_id}:{nonce}`

UUIDs and hashes are preferred in key names; user-visible labels are not treated as authoritative keys.

## Current Behavioral Rules

- Postgres remains SoR even when Redis is healthy.
- cache miss or Redis outage should fall back to the durable path rather than silently changing semantics.
- Pub/Sub provides low-latency fanout; Streams provide durability/replay semantics where needed.
- recent-history cache behavior and watermark rules are defined in `docs/protocol/snapshot.md`.

## TTL Guidance

- `CACHE_TTL_SESSION`
  - aligned with session expiry
- `CACHE_TTL_RECENT_MSGS`
  - default recent-history payload retention window
- `CACHE_TTL_MEMBERS`
  - room membership cache TTL when membership is not purely mutation-driven
- presence keys use heartbeat + grace-window TTL semantics

## Memory / Serialization Notes

- message payload cache is the heaviest key family and should be budgeted with recent-history list length in mind
- recent-history list storage and payload storage should be tuned together, not independently
- JSON strings are the default/debug-friendly serialization choice; a denser encoding is only worth introducing when current payload volume proves it necessary

## Failure Model

- Redis outage:
  - cache paths fall back to Postgres
  - fanout may degrade to narrower/local behavior depending on feature path
  - write-behind can be disabled or shifted to safer fallback operation
- TTL mis-sizing:
  - increases fallback rate and recent-history cache churn
  - should be diagnosed through metrics/logs rather than inferred from client symptoms alone

## Related Docs

- `docs/db/architecture.md`
- `docs/db/write-behind.md`
- `docs/protocol/snapshot.md`
