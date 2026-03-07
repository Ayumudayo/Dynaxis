# tools

Aux binaries + code generation.

## Entry Points
- `tools/wb_worker/main.cpp` + `tools/wb_worker/README.md`: Redis Streams -> Postgres write-behind worker.
- `tools/wb_emit/main.cpp` + `tools/wb_emit/README.md`: emit test events into Redis Streams (`XADD`).
- `tools/wb_check/main.cpp` + `tools/wb_check/README.md`: verify Postgres `session_events` contains an `event_id`.
- `tools/wb_dlq_replayer/main.cpp` + `tools/wb_dlq_replayer/README.md`: replay DLQ stream -> Postgres (and/or dead stream).
- `tools/admin_app/main.cpp` + `tools/admin_app/admin_ui.html`: admin control-plane; canonical doc is `tools/admin_app/README.md` (`/metrics`, `/healthz`, `/readyz`, `/admin`, `/api/v1/auth/context`, `/api/v1/overview`, `/api/v1/instances`, `/api/v1/instances/{instance_id}`, `/api/v1/users`, `/api/v1/sessions/{client_id}`, `/api/v1/users/disconnect`, `/api/v1/users/{mute|unmute|ban|unban|kick}`, `/api/v1/announcements`, `/api/v1/settings`, `/api/v1/ext/inventory`, `/api/v1/ext/precheck`, `/api/v1/ext/deployments`, `/api/v1/ext/schedules`, `/api/v1/worker/write-behind`, `/api/v1/metrics/links`).
- `tools/migrations/runner.cpp` + `tools/migrations/*.sql`: schema migrations.
- `tools/gen_opcodes.py`: generates opcode headers from JSON specs (grouping + `opcode_name()` helpers).
- `tools/gen_opcode_docs.py`: validates opcode specs and generates `docs/protocol/opcodes.md` (system/game share the same 16-bit space).
- `tools/gen_wire_codec.py`: generates `core/include/server/wire/codec.hpp` from `protocol/wire_map.json`.
- `tools/new_plugin.py`: scaffold generator for ChatHook ABI v2 plugin source + manifest.
- `tools/new_script.py`: scaffold generator for Lua cold-hook script + manifest.
- `tools/ext_inventory.py`: manifest inventory scanner/validator for plugin/script artifacts.

## Opcode Codegen
- Specs (JSON): `core/protocol/system_opcodes.json`, `server/protocol/game_opcodes.json`
- Generated headers (C++): `core/include/server/core/protocol/system_opcodes.hpp`, `server/include/server/protocol/game_opcodes.hpp`
- Docs: `tools/gen_opcode_docs.py` -> `docs/protocol/opcodes.md`
- CI check: `python tools/gen_opcode_docs.py --check`

## Related Tool Docs
- `tools/wb_emit/README.md`: emit test events into Redis Streams.
- `tools/wb_check/README.md`: verify Postgres state after write-behind.
- `tools/wb_dlq_replayer/README.md`: reprocess DLQ stream.
- `tools/migrations/README.md`: migration order + operational notes.
- `tools/admin_app/README.md`: canonical admin control-plane API/role/runtime surface.

## Write-behind Pipeline (Streams -> Postgres)
- Produce: `wb_emit` writes events to `REDIS_STREAM_KEY` (default `session_events`).
- Consume: `wb_worker` uses `XREADGROUP` (group `WB_GROUP`, consumer `WB_CONSUMER`) and flushes into Postgres table `session_events`.
- Failure handling:
  - DLQ stream: configurable via `WB_DLQ_STREAM` (default `session_events_dlq`).
  - Dead stream: used by `wb_dlq_replayer` after retry budget (`WB_DEAD_STREAM`, default `session_events_dead`).

## Worker Metrics
`wb_worker` can expose `/metrics` when `METRICS_PORT` is set:
- batch config: `wb_batch_max_events`, `wb_batch_max_bytes`, `wb_batch_delay_ms` (gauges)
- reclaim config: `wb_reclaim_enabled`, `wb_reclaim_interval_ms`, `wb_reclaim_min_idle_ms`, `wb_reclaim_count` (gauges)
- `wb_pending` (gauge)
- reclaim: `wb_reclaim_runs_total`, `wb_reclaim_total`, `wb_reclaim_error_total`, `wb_reclaim_deleted_total` (counters)
- ack: `wb_ack_total`, `wb_ack_fail_total` (counters)
- `wb_flush_total`, `wb_flush_ok_total`, `wb_flush_fail_total`, `wb_flush_dlq_total` (counters)
- `wb_flush_batch_size_last` (gauge)
- `wb_flush_commit_ms_last`, `wb_flush_commit_ms_max` (gauges)
- `wb_flush_commit_ms_sum`, `wb_flush_commit_ms_count` (counters)

## Build (Windows)
```powershell
pwsh scripts/build.ps1 -Config Debug -Target wb_worker
```

## Notes
- Some migrations use `CREATE INDEX CONCURRENTLY`; run those outside transaction blocks (see `tools/migrations/README.md`).
