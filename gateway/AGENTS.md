# gateway (gateway_app)

Edge TCP gateway: accepts clients, authenticates, selects a backend `server_app`, then bridges the TCP streams 1:1.

## Entry Points
- `gateway/src/main.cpp`: process entry.
- `gateway/src/gateway_app.cpp`: app lifecycle + backend selection + metrics server.
- `gateway/src/gateway_connection.cpp`: per-client session; bridges client <-> backend.
- `gateway/src/session_directory.cpp`: sticky routing store (Redis-backed).
- `gateway/include/gateway/auth/`: pluggable authentication.

## Flow
- Accept: `server::core::net::TransportListener` creates `GatewayConnection`(`server::core::net::TransportConnection` 파생) per client.
- Handshake/Auth:
  - `GatewayConnection::on_connect()` starts a handshake deadline (3s) and waits for the first full frame.
  - `GatewayConnection::on_read()` buffers bytes into `prebuffer_` (TCP fragmentation-safe) until `try_finish_handshake()` can decode a full `PacketHeader` + payload.
  - The first frame must be `MSG_LOGIN_REQ`.
    - Extracts `(user, token)` from the payload (length-prefixed UTF-8 fields).
    - Calls `IAuthenticator::authenticate({client_id=user, token, remote_address})`.
    - Derives `client_id_` used for routing/stickiness.
  - Safety limits: handshake buffer cap=64KiB; login payload cap=32KiB.
- Backend select: `GatewayApp::create_backend_connection()` -> `GatewayApp::select_best_server()` returns `SelectedBackend { InstanceRecord, sticky_hit }`.
  - Sticky: `SessionDirectory::find_backend(client_id)` -> prefer the previous `InstanceRecord` when still active.
  - Least connections: sort candidates by `active_sessions` and pick the lowest.
  - Binding is committed post-connect to avoid zombie mappings.
- Post-connect binding: `BackendConnection` calls `GatewayApp::on_backend_connected()` on successful TCP connect.
  - `GatewayApp::on_backend_connected()` -> `SessionDirectory::ensure_backend(client_id, instance_id)` (SETNX + refresh TTL).
- Bridge:
- client -> backend: after handshake, `GatewayConnection` forwards the raw login frame bytes exactly as received, then continues forwarding subsequent reads via `BackendConnection::send()`.
- backend -> client: `BackendConnection::on_read()` -> `GatewayConnection::handle_backend_payload()`.
- Close: either side closing triggers `GatewayConnection::on_disconnect()` / `handle_backend_close()` and `GatewayApp::close_backend_connection()`.

## Run (Standard Runtime = Docker)
```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

## Config (Env Vars)
- Listen/identity: `GATEWAY_LISTEN`, `GATEWAY_ID`
- Discovery/sticky: `REDIS_URI`, `SERVER_REGISTRY_PREFIX`, `SERVER_REGISTRY_TTL`
- Auth: `ALLOW_ANONYMOUS`, `AUTH_PROVIDER`, `AUTH_ENDPOINT`
- Observability: `METRICS_PORT` (serves `/metrics`)

## Metrics
- `/metrics` (Prometheus text format) is served by `server::core::metrics::MetricsHttpServer`.
- Current gateway metrics:
  - `knights_build_info` (gauge; build metadata labels)
  - `gateway_sessions_active` (gauge)
  - `gateway_connections_total` (counter)
