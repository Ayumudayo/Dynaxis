# Observability 가이드

## 1. 메트릭 목표
- **Throughput**: `chat_dispatch_total`, `chat_frame_payload_avg_bytes`, `msgs_per_sec`
- **지연**: `chat_dispatch_latency_*`, `pipeline_latency_ms{stage}`
- **자원**: `chat_session_active`, `chat_job_queue_depth`, `chat_memory_pool_*`
- **에러**: `chat_dispatch_exception_total`, `errors_total{type,stage}`
- **Pub/Sub**: `subscribe_lag_ms`, `publish_total`, `subscribe_total`, `self_echo_drop_total`
- **Load Balancer**: `lb_backend_idle_close_total` (5분 증가분으로 idle 종료 감지)

## 2. 로그 형식
- JSON line: `{"ts": "...", "level": "info", "logger": "...", "msg": "...", "metric": "publish_total", ...}`
- 최소 필드: `ts`, `level`, `server_id`, `gateway_id`, `trace_id`, `span_id`
- 부가 필드: `room`, `user_id`, `session_id`, `opcode`, `latency_ms`
- 운영자는 Fluent Bit/Vector를 통해 로그를 ELK 혹은 ClickHouse로 전달한다.

## 3. 태그 컨벤션
| 태그 | 의미 |
| --- | --- |
| `server_id`, `gateway_id` | 인스턴스 식별자 |
| `env`, `region`, `az` | 배포 환경/리전 |
| `room`, `session_id`, `user_id` | 세션/도메인 정보 |

## 4. Prometheus 수집
- 서버 `/metrics` → `chat_*` 계열
- write-behind 워커 `metric=wb_*` 로그는 Loki 혹은 logging pipeline에서 파싱 후 Prometheus `logfmt` exporter 로 전송
- Load Balancer는 `/metrics` 대신 `metric=lb_backend_idle_close_total` 로그로 동일 정보를 제공하므로, `sum(increase(lb_backend_idle_close_total[5m]))` recording rule을 만든다.

## 5. Grafana 대시보드
- `docker/observability/grafana/dashboards/server-metrics.json`
  - Active Sessions / Dispatch Rate / Job Queue / Memory Pool / Opcode Breakdown
  - “LB Idle Close Rate” 패널:  
    - A: `sum(increase(lb_backend_idle_close_total[5m]))`  
    - B: `lb_backend_idle_close_total` (누적)
- 필요 시 커스텀 패널을 추가하고 버전 관리를 위해 JSON을 Git에 보관한다.

## 6. 경보 규칙 예시
| 지표 | 조건 | 조치 |
| --- | --- | --- |
| `chat_session_active` | 5분 동안 0 | 게이트웨이/로드밸런서 전체 장애 가능성 |
| `chat_dispatch_latency_avg_ms` | 30s > 200ms | DB/Redis 지연 확인 |
| `sum(increase(lb_backend_idle_close_total[5m]))` | 5 이상 | gateway ↔ backend 연결 상태 점검 |
| `wb_pending` | 500 이상 | write-behind 워커 상태 확인 |

## 7. Trace
- OpenTelemetry SDK 사용, OTLP -> Jaeger/Tempo  
- API 서버는 `trace_id`/`span_id` 를 로그에 함께 기록해 상호 참조한다.

## 8. 참고
- `docs/ops/runbook.md` – Alert 대응 순서  
- `docs/ops/fallback-and-alerts.md` – 장애 시 fallback 전략  
- `docs/db/write-behind.md` – write-behind 메트릭 설명
