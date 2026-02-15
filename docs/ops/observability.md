# Observability Guide

목표는 “무슨 문제가 발생했는지 5분 안에 파악”이다. Knights는 기본적으로 Prometheus text format `/metrics` 를 제공하고, `docker/stack`의 `observability` profile로 Prometheus/Grafana를 함께 올릴 수 있다.

## 1. Quick Start (Docker Stack + Observability)

```powershell
pwsh scripts/run_full_stack_observability.ps1
```

기본 접속(Host):
- 게임 트래픽(HAProxy): `127.0.0.1:6000`
- HAProxy stats + metrics: `http://127.0.0.1:8404/` (`/metrics` 포함)
- gateway metrics: `http://127.0.0.1:36001/metrics`, `http://127.0.0.1:36002/metrics`
- server metrics: `http://127.0.0.1:39091/metrics`, `http://127.0.0.1:39092/metrics`
- wb_worker metrics: `http://127.0.0.1:39093/metrics`
- (옵션) Prometheus: `http://127.0.0.1:39090/`
- (옵션) Grafana: `http://127.0.0.1:33000/` (admin password: `GRAFANA_ADMIN_PASSWORD`, 기본 `admin`)

포트는 `docker/stack/docker-compose.yml`의 `*_HOST_PORT` 환경 변수로 재지정할 수 있다.

## 2. Verification Routine (10 minutes)

1) 스택 기동 확인
- Prometheus Targets: `http://127.0.0.1:39090/targets`
- 기대 job: `chat_server`, `gateway`, `write_behind`, `haproxy`, `redis`, `postgres`

```powershell
# (옵션) 빠른 sanity check
pwsh scripts/check_observability.ps1
```

2) 트래픽 주입
- 채팅 트래픽(권장): `client_gui` 또는 `dev_chat_cli`로 로그인/룸 입장/채팅 몇 회 수행
- write-behind roundtrip(도구 기반): `pwsh scripts/smoke_wb.ps1` (Streams -> DB 검증)

3) Grafana 대시보드가 의미 있는지 확인
- `server-metrics.json`: active sessions, dispatch latency(p50/p95/p99), job queue depth가 움직이는지
- `write-behind.json`: `wb_pending`이 감소/평탄화되는지, `wb_flush_*`가 증가하는지
- `infra.json`: redis/postgres exporter가 up인지
- `load-balancer.json`: HAProxy connection/health 추세가 보이는지

## 3. Metrics Catalog (Current)

### server_app
- Build: `knights_build_info{git_hash=...,git_describe=...,build_time_utc=...} 1`
- Sessions: `chat_session_active` (gauge), `chat_session_started_total`, `chat_session_stopped_total`
- Frames: `chat_frame_total`, `chat_frame_error_total`, `chat_frame_payload_*`
- Dispatch: `chat_dispatch_total`, `chat_dispatch_unknown_total`, `chat_dispatch_exception_total`
- Dispatch latency:
  - Gauges: `chat_dispatch_last_latency_ms`, `chat_dispatch_max_latency_ms`, `chat_dispatch_latency_avg_ms`
  - Histogram: `chat_dispatch_latency_ms_bucket`, `chat_dispatch_latency_ms_sum`, `chat_dispatch_latency_ms_count`
- Queues/DB: `chat_job_queue_depth`, `chat_db_job_queue_depth`, `chat_db_job_processed_total`, `chat_db_job_failed_total`
- Fanout/Subscribe: `chat_subscribe_total`, `chat_self_echo_drop_total`, `chat_subscribe_last_lag_ms`
- Per-opcode(hex): `chat_dispatch_opcode_total{opcode="0x0000"}`
- Per-opcode(named): `chat_dispatch_opcode_named_total{opcode="0x0000",name="MSG_*"}`
- Chat hook plugins(실험):
  - `chat_hook_plugins_enabled{mode="..."}` (gauge)
  - `chat_hook_plugin_info{file="...",name="...",version="..."} 1` (gauge)
  - `chat_hook_plugin_reload_attempt_total{file="..."}` / `chat_hook_plugin_reload_success_total{file="..."}` / `chat_hook_plugin_reload_failure_total{file="..."}` (counters)

### gateway_app
- Build: `knights_build_info{...} 1`
- `gateway_sessions_active` (gauge)
- `gateway_connections_total` (counter)

### wb_worker
- Build: `knights_build_info{...} 1`
- Backlog: `wb_pending` (gauge)
- Flush: `wb_flush_total`, `wb_flush_ok_total`, `wb_flush_fail_total`, `wb_flush_dlq_total` (counters)
- Batch/Latency: `wb_flush_batch_size_last` (gauge), `wb_flush_commit_ms_last` (gauge)

## 4. PromQL Snippets

```promql
# 모든 타겟이 up 인지 빠르게 확인
sum(up)

# 배포된 빌드 버전 라벨 확인
knights_build_info

# server_app dispatch p95 (traffic가 있어야 NaN이 아님)
histogram_quantile(0.95, sum by (le) (rate(chat_dispatch_latency_ms_bucket[5m])))

# write-behind backlog (최근 5분 max)
max_over_time(wb_pending[5m])
```

## 5. Troubleshooting

- p95/p99가 NaN: 최근 rate window에 샘플이 없으면 정상적으로 NaN이 나올 수 있다. (트래픽 주입 후 재확인)
- No data: `/metrics` 엔드포인트(Host port mapping)와 Prometheus Targets를 먼저 확인한다.
- redis/postgres exporter down: `docker/stack/docker-compose.yml`의 `observability` profile이 올라왔는지 확인한다.

## 6. Tracing (Roadmap)

OpenTelemetry/OTLP는 아직 `docker/stack` 표준 런타임에 포함되어 있지 않다. `/metrics` + structured log를 우선 기준으로 하고, tracing은 이후 단계에서 추가한다.
