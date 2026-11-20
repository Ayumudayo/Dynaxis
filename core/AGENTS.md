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

## Build boundary
- The default full profile retains the MSA chat server and the 4.0 Stable API.
- Ownership and migration limits: `docs/architecture/core-and-msa.md`.

## Build / Test (Windows / Linux)
```powershell
python scripts/build.py --profile core --config Debug --step all
python scripts/build.py --profile core --config Debug --step test
```

- Keep host modules domain-agnostic: no `server/`, `gateway/`, `client_gui/`, or `admin_console/` implementation coupling from `core/src`.

## Local Constraints
- Keep dependencies one-way: `server/` and `gateway/` depend on `core/`, not the other way around.
- If you add new metrics, keep names Prometheus-friendly (`snake_case`, `_total`, `_ms`, etc.).

See `core/include/AGENTS.md` (public headers) and `core/src/AGENTS.md` (implementation) for module layout.
