# Observability Guide (Expanded)

이 문서는 Knights 스택의 계측 전략을 상세히 설명한다. 목표는 “무슨 문제가 발생했는지 5분 안에 파악”하는 것이다.

## 1. 메트릭 카탈로그
| 계열 | 이름 | 유형 | 설명 |
| --- | --- | --- | --- |
| Chat | `chat_dispatch_total` | Counter | opcode 처리 누계 |
|  | `chat_dispatch_latency_*` | Gauge/Counter | 라우팅 지연 (last/max/sum/count) |
|  | `chat_session_active` | Gauge | 현재 연결 수 |
|  | `chat_job_queue_depth` | Gauge | JobQueue 길이 |
| Pub/Sub | `chat_subscribe_total`, `chat_subscribe_last_lag_ms` | Counter/Gauge | Redis Stream lag |
| Load Balancer | `lb_backend_idle_close_total` | Counter(log) | backend 유휴 종료 횟수 |
| Write-behind | `wb_pending`, `wb_flush_*` | Gauge/Log | Redis Stream backlog, flush latency |
| DLQ | `wb_dlq_replay_*` | Counter | DLQ 처리 상태 |

### Prometheus 스크랩
- 서버 `/metrics`: `server_app`, `wb_worker`
- 로그 기반: `metric=lb_backend_idle_close_total`, `metric=wb_dlq_replay*` 는 Loki/logfmt exporter로 파싱 → Prometheus `lb_backend_idle_close_total` 계열로 변환
- Recording Rule 예시:
```yaml
- record: job:lb_idle_close_5m
  expr: sum(increase(lb_backend_idle_close_total[5m]))
```

## 2. Grafana 대시보드
- `docker/observability/grafana/dashboards/server-metrics.json`
  - Panel 1: Active Sessions (stat)
  - Panel 2: Dispatch Rate
  - Panel 3: Job Queue Depth
  - Panel 4: Memory Pool Usage
  - Panel 5: Opcode Table
  - Panel 6: **LB Idle Close Rate** (A=5분 증가분, B=누적)
- 대시보드 버전을 Git으로 관리하고, 변경 시 `version` 필드를 증가시킨다.

## 3. 로그 형식
```json
{"ts":"2025-11-10T01:00:00Z","level":"info","logger":"server_app","metric":"chat_dispatch_total","opcode":"0x0005","room":"lobby"}
```
- 필수 필드: `ts`, `level`, `logger`, `metric`
- 선택 필드: `room`, `session_id`, `gateway_id`, `action`
- logfmt exporter 예시: `metric=lb_backend_idle_close_total value=1 backend=server-1` → Prometheus `lb_backend_idle_close_total{backend="server-1"}`

## 4. Alerting 전략
| 이름 | PromQL | 조건 | Runbook |
| --- | --- | --- | --- |
| LB Idle Spike | `sum(increase(lb_backend_idle_close_total[5m]))` | > 5 | `docs/ops/fallback-and-alerts.md` |
| Redis Pub/Sub Lag | `chat_subscribe_last_lag_ms{quantile="0.95"}` | > 200ms | 같은 문서 |
| WB Backlog | `wb_pending` | > 500 | DLQ 가이드 |
| Dispatch Errors | `sum(rate(chat_dispatch_exception_total[1m]))` | > 1 | runbook |

AlertManager → Slack/Webhook → On-call 순으로 전달하며, 각 알람에는 runbook 링크를 포함한다.

## 5. Trace
- OpenTelemetry SDK 사용, OTLP → Jaeger/Tempo
- Trace ID 를 로그와 메트릭에 함께 남기기 위해 `trace_id`, `span_id` 필드를 삽입
- 샘플링 정책: 1% 기본, 장애 시 100% 로 임시 조정 가능 (환경 변수 `OTEL_TRACES_SAMPLER_ARG`)

## 6. 데이터 수명 주기
- Prometheus: 30일 보관, 장기 분석은 Thanos/Promscale 로 downsample
- 로그: S3/Glacier 에 90일 보관
- DLQ/Replayer 로그: 7일 보관 후 압축

## 7. 참고
- `docs/ops/runbook.md`
- `docs/ops/fallback-and-alerts.md`
- `docs/ops/deployment.md`
