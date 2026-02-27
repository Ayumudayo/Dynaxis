# 관측성 가이드

목표는 “무슨 문제가 발생했는지 5분 안에 파악”이다. Knights는 기본적으로 Prometheus 텍스트 포맷 `/metrics`를 제공하고, `docker/stack`의 `observability` 프로필로 Prometheus/Grafana를 함께 올릴 수 있다.

## 1. 빠른 시작 (Docker Stack + 관측성)

```powershell
pwsh scripts/run_full_stack_observability.ps1
```

기본 접속(호스트):
- 게임 트래픽(HAProxy): `127.0.0.1:6000`
- HAProxy 통계(stats) + 메트릭(metrics): `http://127.0.0.1:8404/` (`/metrics` 포함)
- gateway 메트릭: `http://127.0.0.1:36001/metrics`, `http://127.0.0.1:36002/metrics`
- server 메트릭: `http://127.0.0.1:39091/metrics`, `http://127.0.0.1:39092/metrics`
- wb_worker 메트릭: `http://127.0.0.1:39093/metrics`
- 관리자 콘솔(UI): `http://127.0.0.1:39200/admin`
- 관리자 API/metrics: `http://127.0.0.1:39200/api/v1/overview`, `http://127.0.0.1:39200/metrics`
- (health/ready) 각 관리자 포트는 `/healthz`, `/readyz`도 제공한다. (`server_app`은 `/logs`도 제공)
- (옵션) Prometheus: `http://127.0.0.1:39090/`
- (옵션) Grafana: `http://127.0.0.1:33000/` (관리자 비밀번호: `GRAFANA_ADMIN_PASSWORD`, 기본 `admin`)

포트는 `docker/stack/docker-compose.yml`의 `*_HOST_PORT` 환경 변수로 재지정할 수 있다.

## 2. 검증 루틴 (10분)

1) 스택 기동 확인
- Prometheus Targets: `http://127.0.0.1:39090/targets`
- 기대 job(잡): `chat_server`, `gateway`, `write_behind`, `admin_app`, `haproxy`, `redis`, `postgres`

```powershell
# (옵션) 빠른 기본 점검
pwsh scripts/check_observability.ps1

# (옵션) 서비스별 /metrics payload 스모크 점검
pwsh scripts/smoke_metrics.ps1
```

2) 트래픽 주입
- 채팅 트래픽(권장): `client_gui` 또는 `dev_chat_cli`로 로그인/룸 입장/채팅 몇 회 수행
- write-behind 왕복 검증(roundtrip, 도구 기반): `pwsh scripts/smoke_wb.ps1` (Streams -> DB 검증)

3) Grafana 대시보드 유효성 확인
- `server-metrics.json`: 활성 세션, 디스패치(dispatch) 지연(p50/p95/p99), 작업 큐 깊이가 움직이는지
- `write-behind.json`: `wb_pending`이 감소/평탄화되는지, `wb_flush_*`가 증가하는지
- `infra.json`: redis/postgres exporter가 up인지
- `load-balancer.json`: HAProxy connection/health 추세가 보이는지
- `gateway-udp-quality.json`: delivery별 forward, reason별 drop, jitter/rtt/loss 추세가 보이는지

## 3. 메트릭 목록 (현재)

### 공통(core)
- 빌드(Build): `knights_build_info{git_hash=...,git_describe=...,build_time_utc=...} 1`
- 런타임 핵심(core runtime):
  - `core_runtime_accept_total` (counter)
  - `core_runtime_session_started_total` (counter)
  - `core_runtime_session_stopped_total` (counter)
  - `core_runtime_session_active` (gauge)
  - `core_runtime_dispatch_total`, `core_runtime_dispatch_unknown_total`, `core_runtime_dispatch_exception_total` (counters)
  - `core_runtime_send_queue_drop_total`, `core_runtime_packet_error_total` (counters)

### 서버 앱(server_app)
- 세션: `chat_session_active` (gauge), `chat_session_started_total`, `chat_session_stopped_total`
- 세션 타임아웃: `chat_session_timeout_total`, `chat_session_write_timeout_total` (counters)
- 프레임: `chat_frame_total`, `chat_frame_error_total`, `chat_frame_payload_*`
- 디스패치: `chat_dispatch_total`, `chat_dispatch_unknown_total`, `chat_dispatch_exception_total`
- 디스패치 지연:
  - 게이지(Gauges): `chat_dispatch_last_latency_ms`, `chat_dispatch_max_latency_ms`, `chat_dispatch_latency_avg_ms`
  - 히스토그램(Histogram): `chat_dispatch_latency_ms_bucket`, `chat_dispatch_latency_ms_sum`, `chat_dispatch_latency_ms_count`
- 큐/DB: `chat_job_queue_depth`, `chat_db_job_queue_depth`, `chat_db_job_processed_total`, `chat_db_job_failed_total`
- 팬아웃/구독(Fanout/Subscribe): `chat_subscribe_total`, `chat_self_echo_drop_total`, `chat_subscribe_last_lag_ms`
- opcode별(hex): `chat_dispatch_opcode_total{opcode="0x0000"}`
- opcode별(이름): `chat_dispatch_opcode_named_total{opcode="0x0000",name="MSG_*"}`
- Chat hook 플러그인(실험):
  - `chat_hook_plugins_enabled{mode="..."}` (gauge)
  - `chat_hook_plugin_info{file="...",name="...",version="..."} 1` (gauge)
  - `chat_hook_plugin_reload_attempt_total{file="..."}` / `chat_hook_plugin_reload_success_total{file="..."}` / `chat_hook_plugin_reload_failure_total{file="..."}` (counters)

### 게이트웨이 앱(gateway_app)
- `gateway_sessions_active` (gauge)
- `gateway_connections_total` (counter)
- 백엔드(Backend) 신뢰성:
  - `gateway_backend_resolve_fail_total` (counter)
  - `gateway_backend_connect_fail_total` (counter)
  - `gateway_backend_connect_timeout_total` (counter)
  - `gateway_backend_write_error_total` (counter)
  - `gateway_backend_send_queue_overflow_total` (counter)
- 백엔드(Backend) 가드레일 설정:
  - `gateway_backend_connect_timeout_ms` (gauge)
  - `gateway_backend_send_queue_max_bytes` (gauge)
- UDP 수신/바인드(ingress/bind) 가드레일:
  - `gateway_udp_enabled` (gauge)
  - `gateway_udp_packets_total`, `gateway_udp_receive_error_total` (counters)
  - `gateway_udp_bind_ticket_issued_total`, `gateway_udp_bind_success_total`, `gateway_udp_bind_reject_total` (counters)
  - `gateway_udp_bind_rate_limit_reject_total`, `gateway_udp_bind_block_total` (counters)
  - `gateway_udp_forward_total`, `gateway_udp_replay_drop_total`, `gateway_udp_reorder_drop_total`, `gateway_udp_duplicate_drop_total`, `gateway_udp_retransmit_total` (counters)
  - `gateway_udp_loss_estimated_total` (counter; seq-gap 기반 추정치)
  - `gateway_udp_jitter_ms_last`, `gateway_udp_rtt_ms_last` (gauges; latest observed)
  - `gateway_udp_bind_ttl_ms`, `gateway_udp_bind_fail_window_ms`, `gateway_udp_bind_fail_limit`, `gateway_udp_bind_block_ms` (gauges)

### 워커(wb_worker)
- 백로그: `wb_pending` (gauge)
- DB 재연결 설정/백오프:
  - `wb_db_reconnect_base_ms`, `wb_db_reconnect_max_ms` (gauges)
  - `wb_db_reconnect_backoff_ms_last` (gauge)
- DB 가용성/드롭 신호:
  - `wb_db_unavailable_total` (counter)
  - `wb_error_drop_total` (counter)
- 플러시(Flush): `wb_flush_total`, `wb_flush_ok_total`, `wb_flush_fail_total`, `wb_flush_dlq_total` (counters)
- 배치/지연: `wb_flush_batch_size_last` (gauge), `wb_flush_commit_ms_last` (gauge)

### 관리자 앱(admin_app)
- 빌드(Build): `knights_build_info{...} 1`
- API 트래픽: `admin_http_requests_total`, `admin_http_errors_total`, `admin_http_server_errors_total` (counters)
- 인증 트래픽: `admin_http_unauthorized_total`, `admin_http_forbidden_total` (counters)
- API 종류별: `admin_overview_requests_total`, `admin_instances_requests_total`, `admin_session_lookup_requests_total`, `admin_worker_requests_total` (counters)
- 폴링/캐시: `admin_poll_errors_total` (counter), `admin_instances_cached` (gauge)
- 의존성/상태: `admin_redis_available`, `admin_worker_metrics_available`, `admin_read_only_mode` (gauges)

## 4. PromQL 예시

```promql
# 모든 타겟이 up 인지 빠르게 확인
sum(up)

# 배포된 빌드 버전 라벨 확인
knights_build_info

# 서버 앱 디스패치(server_app) p95 (트래픽이 있어야 NaN이 아님)
histogram_quantile(0.95, sum by (le) (rate(chat_dispatch_latency_ms_bucket[5m])))

# 쓰기 지연(write-behind) 백로그 (최근 5분 최대값 max)
max_over_time(wb_pending[5m])
```

## 5. 문제 해결

- p95/p99가 NaN: 최근 rate window에 샘플이 없으면 정상적으로 NaN이 나올 수 있다. (트래픽 주입 후 재확인)
- 데이터 없음(No data): `/metrics` 엔드포인트(호스트 포트 매핑)와 Prometheus Targets를 먼저 확인한다.
- redis/postgres exporter down: `docker/stack/docker-compose.yml`의 `observability` profile이 올라왔는지 확인한다.
- chat 상세 로그가 기대보다 적음: 최신 서버 경로에서는 고빈도 로그(`CHAT_SEND` 본문, whisper 상태, publish 카운트)가 노이즈 절감을 위해 `debug` 또는 샘플링으로 조정되어 기본 `info`에서 보이지 않을 수 있다.

## 6. 경보 규칙 (Gateway UDP)

- Prometheus rule 파일(file): `docker/observability/prometheus/alerts.yml`
- 기본 경보:
  - `GatewayUdpBindAbuseSpike`: bind rate-limit reject 급증
  - `GatewayUdpEstimatedLossHigh`: 추정 loss ratio > 5%
  - `GatewayUdpReplayDropSpike`: replay/reorder drop 급증
  - `GatewayUdpJitterHigh`: jitter 지속 고수준

## 7. 트레이싱 (로드맵)

OpenTelemetry/OTLP는 아직 `docker/stack` 표준 런타임에 포함되어 있지 않다. `/metrics` + 구조화 로그를 우선 기준으로 하고, tracing은 이후 단계에서 추가한다.
