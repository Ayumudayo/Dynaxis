# server (server_app)

Chat logic node. Owns room/user state, storage access, and Redis instance registry.

## Entry Points
- `server/src/main.cpp`: process entry; calls into bootstrap.
- `server/src/app/bootstrap.cpp`: wiring (io_context/threads/listener/storage/registry).
- `server/src/app/router.cpp`: opcode -> handler routing table.
- `server/src/chat/chat_service_core.cpp`: main chat service state + orchestration.
- `server/src/chat/handlers_*.cpp`: opcode handlers.
- `server/src/state/instance_registry.cpp`: Redis instance registry (discovery for gateways).
- `server/src/app/metrics_server.cpp`: Prometheus `/metrics` endpoint (port via `METRICS_PORT`).

## Flow
- Bootstrap: `server::app::run_server()` loads `ServerConfig`, installs crash handler, and wires explicit dependencies through instance context and `ChatService::Dependencies`.
- Core runtime: `asio::io_context`, `JobQueue`, `ThreadManager`, `BufferManager`, `Dispatcher`, `SessionOptions`, `ConnectionRuntimeState`.
- Storage:
  - Postgres pool: `server::storage::postgres::make_connection_pool()` + periodic health checks.
  - Redis client: `server::storage::redis::make_redis_client()` + periodic health checks.
- Discovery: if Redis is present, upsert `InstanceRecord` into `RedisInstanceStateBackend` and refresh via scheduled heartbeat.
- Routing: `register_routes(dispatcher, chat)` binds opcodes to `ChatService` handlers.
- Ingress: `core::net::SessionListener` accepts sessions and hands frames to `Dispatcher`.
- Fanout: optional Redis Pub/Sub `psubscribe` on `${REDIS_CHANNEL_PREFIX}fanout:*` for distributed room broadcasts.
- Observability: optional `MetricsServer` starts when `METRICS_PORT` is set.

## Run (Standard Runtime = Docker)
```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build

# Stack + Prometheus/Grafana
pwsh scripts/run_full_stack_observability.ps1
```

## Config (Env Vars)
- Common: `PORT`, `LOG_BUFFER_CAPACITY`
- Storage: `DB_URI` (unset -> DB features disabled)
- Discovery: `SERVER_INSTANCE_ID`, `SERVER_ADVERTISE_HOST`, `SERVER_ADVERTISE_PORT`, `SERVER_HEARTBEAT_INTERVAL`, `SERVER_REGISTRY_PREFIX`, `SERVER_REGISTRY_TTL`, `SERVER_ROLE`, `SERVER_REGION`, `SERVER_SHARD`, `SERVER_TAGS`
- DB: `DB_POOL_MIN`, `DB_POOL_MAX`, `DB_CONN_TIMEOUT_MS`, `DB_QUERY_TIMEOUT_MS`, `DB_PREPARE_STATEMENTS`, `DB_WORKER_THREADS`
- Redis: `REDIS_URI`, `REDIS_POOL_MAX`, `REDIS_USE_STREAMS`, `PRESENCE_CLEAN_ON_START`, `REDIS_CHANNEL_PREFIX`, `USE_REDIS_PUBSUB`, `GATEWAY_ID`
- Observability: `METRICS_PORT`

## Notes
- Dispatch latency quantiles can be NaN if there are no samples in the PromQL rate window (normal; send traffic).
- Instance registry uses Redis and the `SERVER_REGISTRY_PREFIX` keyspace.
