# docker/observability

Prometheus + Grafana configuration (used by the `observability` compose profile).

## Prometheus
- Config: `docker/observability/prometheus/prometheus.yml`
- Alerts: `docker/observability/prometheus/alerts.yml`
- Alert rule tests: `docker/observability/prometheus/alerts.tests.yml`
- Jobs: `chat_server`, `gateway`, `write_behind`, `admin_app`, `haproxy`, `redis`, `postgres`

## Grafana
- Datasource provisioning: `docker/observability/grafana/provisioning/datasources/prometheus.yml`
- Dashboard provisioning: `docker/observability/grafana/provisioning/dashboards/dashboard.yml`
- Dashboards (JSON):
  - `docker/observability/grafana/dashboards/infra.json`
  - `docker/observability/grafana/dashboards/load-balancer.json`
  - `docker/observability/grafana/dashboards/server-metrics.json`
  - `docker/observability/grafana/dashboards/write-behind.json`

## Local URLs (Defaults)
- Prometheus: `http://127.0.0.1:39090/`
- Grafana: `http://127.0.0.1:33000/` (admin password: `GRAFANA_ADMIN_PASSWORD`, default `admin`)

## Quick Checks
```powershell
pwsh scripts/check_observability.ps1
pwsh scripts/check_prometheus_rules.ps1
```

## Workflow: Add a Metric
1) Export it from the service on `/metrics`.
2) Add/adjust scrape targets in `docker/observability/prometheus/prometheus.yml` if needed.
3) Update/add a dashboard JSON under `docker/observability/grafana/dashboards/`.

## Notes
- Most binaries export `runtime_build_info` (git hash/describe + build time) at the top of `/metrics`.
- `server_app` can export chat hook plugin metrics when plugins are enabled.
