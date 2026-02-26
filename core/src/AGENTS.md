# core/src

Implementation of `server_core` (shared library). Keep this layer reusable: services (`server/`, `gateway/`) depend on `core/`, never the other way around.

## Layout
```
core/src/
├─ concurrent/        # thread manager / schedulers
├─ metrics/           # `/metrics` HTTP server implementation
├─ net/               # Hive/listener/connection/session implementations
├─ storage/           # shared storage helpers (non-service-specific)
├─ util/              # logging, crash handler, paths
└─ runtime_metrics.cpp  # counters + dispatch latency histogram aggregation
```

## Where To Look
- `core/src/runtime_metrics.cpp`: histogram bucket updates + snapshot export.
- `core/src/metrics/http_server.cpp`: request parsing + `/metrics` response wiring.
- `core/src/net/`: accept/connect and dispatcher plumbing used by gateway/server.
- `core/src/util/log.cpp`: logging backend (avoid PII/secrets).

## Conventions
- Add new files to `core/CMakeLists.txt` explicitly (no `file(GLOB ...)`).
- Keep build portability: Docker/Linux uses system deps; Windows uses vcpkg feature `windows-dev`.

## Anti-Patterns
- No references to `server/*` or `gateway/*` code from `core/src`.
- Avoid blocking work on metrics server thread; keep callbacks fast.
