# docker/stack

`docker/stack/docker-compose.yml` brings up a full verification stack:

Client (host) -> `haproxy` (TCP) -> `gateway-*` -> `server-*`.

## Key Files
- `docker/stack/docker-compose.yml`: service graph + host port mappings.
- `docker/stack/haproxy/haproxy.cfg`: TCP LB configuration.
- `docker/stack/README.md`: quick start + default URLs.

## Services (Core)
- `postgres`, `redis`: state/storage.
- `migrator`: runs schema migrations before app services start.
- `server-1`, `server-2`: chat servers (`METRICS_PORT=9090` mapped to host `39091/39092`).
- `gateway-1`, `gateway-2`: gateways (`METRICS_PORT=6001` mapped to host `36001/36002`).
- `wb_worker`: write-behind worker (`METRICS_PORT=9090` mapped to host `39093`).
- `admin-app`: read-only admin control-plane (`METRICS_PORT=39200` mapped to host `39200`, UI at `/admin`).
- `haproxy`: game traffic on host `6000`, stats+Prometheus metrics on host `8404`.

## Chat Hook Plugins (Experimental)
- `server-*` defaults to `CHAT_HOOK_ENABLED=0` and `CHAT_HOOK_PLUGINS_DIR=/app/plugins` (see `docker/stack/docker-compose.yml`).
- Sample plugins are packaged into the image under `/app/plugins_builtin/` (fallback path).
- Hot reload is done by swapping the `.so` in-place (optional lock/sentinel file to defer reload).

## Lua (Experimental)
- `server-*` defaults to `LUA_ENABLED=0` with scripts mounted at `/app/scripts` and fallback `/app/scripts_builtin`.

## Observability Profile
Enables: `prometheus`, `grafana`, `redis_exporter`, `postgres_exporter`.

## Commands
```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability
pwsh scripts/deploy_docker.ps1 -Action down
```

## Port Overrides
Most host ports are configurable via env vars in `docker/stack/docker-compose.yml`:
- `HAPROXY_HOST_PORT`, `HAPROXY_STATS_HOST_PORT`
- `PROMETHEUS_HOST_PORT`, `GRAFANA_HOST_PORT`
- `REDIS_EXPORTER_HOST_PORT`, `POSTGRES_EXPORTER_HOST_PORT`
