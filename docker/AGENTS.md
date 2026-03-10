# docker

Linux(Docker) full-stack runtime.

Prefer `scripts/deploy_docker.ps1` over raw `docker compose` so base image building and compose profiles stay consistent.

## Entry Points
- `docker/stack/docker-compose.yml`: verification stack (HAProxy -> gateway -> server, plus Redis/Postgres).
- `docker/stack/haproxy/haproxy.cfg`: TCP LB + stats/metrics endpoint.
- `docker/observability/prometheus/prometheus.yml`: Prometheus scrape config (observability profile).
- `docker/observability/grafana/provisioning/`: Grafana provisioning.
- `docker/observability/grafana/dashboards/*.json`: dashboards.

## Commands
```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
pwsh scripts/deploy_docker.ps1 -Action down
pwsh scripts/run_full_stack_observability.ps1
```

## Notes
- Default compose project name: `dynaxis-stack` (override via `scripts/deploy_docker.ps1 -ProjectName ...`).
- Observability is gated behind compose profile `observability` (toggle via `scripts/deploy_docker.ps1 -Observability`).

See `docker/stack/AGENTS.md` and `docker/observability/AGENTS.md` for module-specific details.
