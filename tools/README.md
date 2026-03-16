# Tools Index

This directory contains operational and validation tools used around the Dynaxis stack.
Each tool keeps its detailed usage in its own README when the surface is non-trivial.

## Core Tool Entry Points

| Tool | Path | Purpose |
| --- | --- | --- |
| Load Generator | `tools/loadgen/` | transport-aware soak, latency, and proof scenarios |
| Admin App | `tools/admin_app/` | admin/control-plane API and UI |
| Migrations | `tools/migrations/` | schema migration runner |
| Provisioning | `tools/provisioning/` | database/bootstrap SQL helpers |
| Write-Behind Worker | `tools/wb_worker/` | Redis Streams -> Postgres persistence worker |
| Write-Behind Emitter | `tools/wb_emit/` | emits test/runtime events into the write-behind path |
| Write-Behind Checker | `tools/wb_check/` | verifies write-behind persistence outcomes |
| DLQ Replayer | `tools/wb_dlq_replayer/` | replays dead-lettered write-behind events |

## Supporting Scripts

- `tools/gen_opcodes.py` - generate opcode headers
- `tools/gen_wire_codec.py` - generate wire codec headers
- `tools/gen_opcode_docs.py` - regenerate opcode documentation
- `tools/check_core_api_contracts.py` - core API governance/boundary checks
- `tools/check_doxygen_coverage.py` - Doxygen coverage validation
- `tools/check_markdown_links.py` - local markdown link smoke for repository docs

## Related Docs

- `docs/README.md` - canonical documentation index
- `docs/tests.md` - verification entrypoint
