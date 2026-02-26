# core/include

Public header surface for `server_core` (consumed by `server_app`, `gateway_app`, tools, and `client_gui`).

## Layout
```
core/include/
└─ server/
   ├─ core/               # primary API surface (`server::core::*`)
   │  ├─ net/             # Hive/listener/connection/session abstractions
   │  ├─ metrics/         # `/metrics` HTTP server + Prometheus helpers
   │  ├─ util/            # log/paths/service_registry/crash handler
   │  └─ runtime_metrics.hpp  # process-wide counters + dispatch latency histogram buckets
   └─ wire/
      └─ codec.hpp        # generated wire codec (see protocol JSON + generators)
```

## Where To Look
- `core/include/server/core/runtime_metrics.hpp`: metric storage interface + snapshot types.
- `core/include/server/core/metrics/http_server.hpp`: minimal `/metrics` HTTP server.
- `core/include/server/core/net/`: connection lifecycle primitives shared by gateway/server.
- `core/include/server/core/util/log.hpp`: logging API (keep secrets out).
- `core/include/server/core/protocol/`: generated opcodes + packet/header helpers.

## Conventions
- Namespace mirrors path: `core/include/server/core/net/*` -> `server::core::net`.
- Keep headers lightweight: prefer forward declarations; avoid pulling in service-only headers.
- Prometheus names use `snake_case` + conventional suffixes (`_total`, `_ms`, `_bytes`).

## Anti-Patterns
- `core/include/` must not depend on `server/` or `gateway/` implementation headers.
- Don’t edit generated headers directly (edit JSON + generator instead).
