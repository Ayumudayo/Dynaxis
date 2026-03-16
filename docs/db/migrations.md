# Migration Strategy

Dynaxis uses versioned SQL files plus a lightweight runner.
This document is the current migration entrypoint; older planning/checklist notes are not part of the active docs tree anymore.

## Current Layout

- SQL files: `tools/migrations/`
- naming pattern:
  - `0001_*.sql`
  - `0002_*.sql`
  - ...
- applied versions are tracked in `schema_migrations`

## Runner Behavior

- reads the applied version set from `schema_migrations`
- applies remaining files in filename order
- records a version only after the SQL for that version succeeds
- supports a dry-run style listing before execution
- runs `CREATE INDEX CONCURRENTLY` style work outside a transaction where required

## Current Migration Set

- `0001_schema.sql` / `0001_init.sql` class:
  - core schema creation
- `0002_indexes.sql` class:
  - index/extension additions
- `0003_identity.sql` class:
  - identity-related schema adjustments
- `0004_session_events.sql` class:
  - write-behind/session-event persistence

## Usage

Windows example:

```powershell
build-windows/Debug/migrations_runner.exe --db-uri "postgresql://user:pass@127.0.0.1:5432/appdb?sslmode=disable" --dry-run
build-windows/Debug/migrations_runner.exe --db-uri "postgresql://user:pass@127.0.0.1:5432/appdb?sslmode=disable"
```

Direct `psql` examples and quick local setup live in `docs/getting-started.md`.

## Operational Rules

- migration ordering is filename-driven and monotonic
- destructive changes should be introduced deliberately and not hidden inside mixed-purpose SQL files
- long-running index creation should use the appropriate non-transactional path
- schema evolution should keep the active application/runtime docs aligned in the same change

## Related Docs

- `docs/db/architecture.md`
- `docs/db/write-behind.md`
- `docs/getting-started.md`
