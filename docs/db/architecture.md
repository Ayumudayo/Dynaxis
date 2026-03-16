# DB Architecture

This document records the current storage/runtime architecture used by Dynaxis.
PostgreSQL remains the system of record, while Redis provides low-latency cache, fanout, continuity, and write-behind support.

## Current Ownership

- `server_core` owns the generic storage/runtime seams:
  - transaction boundary
  - connection pool contract
  - async DB execution seam
  - shared Redis client contract
- `server/` owns chat-domain repository DTOs and concrete Postgres/Redis implementations
- `gateway/` and `tools/` consume the shared Redis/storage seams but do not own the database model

## Runtime Model

- PostgreSQL is the source of record for durable user, room, membership, message, and session state.
- Redis is used for:
  - continuity/session cache
  - presence and room membership cache
  - Pub/Sub fanout
  - recent-history cache
  - write-behind streams
  - rate-limit / idempotency keys
- DB work stays off the main `io_context`; runtime code uses worker threads and explicit transaction boundaries.

## Current Data Model

The live schema centers on:

- `users`
- `rooms`
- `memberships`
- `messages`
- `sessions`
- `session_events`
- `schema_migrations`

Canonical identifiers are UUIDs for `user_id`, `room_id`, and `session_id`.
Names remain labels/search fields rather than authoritative identity.

## Security And Operational Baseline

- password hashes use Argon2id
- session tokens are stored as hashes (`token_hash`), not plaintext
- DB access should use app-scoped least-privilege credentials
- `DB_URI` and other secrets are injected through the runtime environment, not committed files
- recovery and observability should treat DB degradation as a first-class runtime concern through readiness/metrics/logs

## Related Current-State Docs

- cache/session/fanout strategy: `docs/db/redis-strategy.md`
- write-behind runtime: `docs/db/write-behind.md`
- migration strategy and runner usage: `docs/db/migrations.md`
- local setup and first-run flow: `docs/getting-started.md`
