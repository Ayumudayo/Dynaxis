# core (server_core)

Shared C++20 library used by `server_app`, `gateway_app`, tools, and the Windows dev client.

## Entry Points
- `core/CMakeLists.txt`: defines the `server_core` library target (explicit source list; no GLOB).
- `core/include/server/core/build_info.hpp`: build metadata (git hash/describe + build time).
- `core/include/server/core/metrics/build_info.hpp`: Prometheus helper for `runtime_build_info`.
- `core/include/server/core/runtime_metrics.hpp`: process-wide counters + dispatch latency histogram buckets.
- `core/src/runtime_metrics.cpp`: runtime metrics storage/aggregation.
- `core/include/server/core/metrics/http_server.hpp`: minimal HTTP server for exposing `/metrics`.
- `core/src/metrics/http_server.cpp`: `MetricsHttpServer` implementation.
- `core/include/server/core/plugin/shared_library.hpp`: cross-platform shared library wrapper for plugin loading.
- `core/include/server/core/plugin/plugin_host.hpp`: generic hot-reload host for single plugin API table.
- `core/include/server/core/plugin/plugin_chain_host.hpp`: multi-plugin chain host (directory scan + stable ordering).
- `core/include/server/core/scripting/script_watcher.hpp`: mtime-based watcher with lock/sentinel support.
- `core/include/server/core/scripting/lua_runtime.hpp`: Lua runtime facade and call/load metrics surface.
- `core/include/server/core/scripting/lua_sandbox.hpp`: safe environment and resource limit helpers.

## Protocol / Wire Codegen
- Sources: `core/protocol/system_opcodes.json`, `server/protocol/game_opcodes.json`, `protocol/wire_map.json`
- Generators: `tools/gen_opcodes.py`, `tools/gen_wire_codec.py`
- Tracked forwarding headers:
  - `core/include/server/core/protocol/system_opcodes.hpp`
  - `core/include/server/protocol/game_opcodes.hpp`
  - `core/include/server/wire/codec.hpp`
- Build/install generated payload:
  - `build/generated/include/server/generated/**`
- Rule: treat generated payload as write-only; edit the JSON + generator instead.

## Build / Test (Windows)
```powershell
pwsh scripts/build.ps1 -Config Debug -Target server_core
ctest --preset windows-test
```

## Runtime Extensibility Notes
- Plugin/scripting modules in `core/include/server/core/plugin/*` and `core/include/server/core/scripting/*` are currently governed as transitional public API; check `docs/core-api-boundary.md` before signature changes.
- Keep host modules domain-agnostic: no `server/` or `gateway/` implementation coupling from `core/src`.
- If adding plugin or Lua metrics, keep naming aligned with existing series in `server/src/app/metrics_server.cpp`.

## Local Constraints
- Keep dependencies one-way: `server/` and `gateway/` depend on `core/`, not the other way around.
- If you add new metrics, keep names Prometheus-friendly (`snake_case`, `_total`, `_ms`, etc.).

See `core/include/AGENTS.md` (public headers) and `core/src/AGENTS.md` (implementation) for module layout.
