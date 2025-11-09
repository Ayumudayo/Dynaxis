# Fallback 및 알림 전략

장애 상황에서 서비스가 중단되지 않도록 하기 위해 다음과 같은 fallback/알람 체계를 사용한다.

## 1. Redis/DB 장애
- **Redis**  
  - sticky session은 Redis를 신뢰하지만, 장애 시 `SessionDirectory` 는 로컬 캐시에 있는 값을 한 번 더 사용한 후 fallback backend를 새로 선택한다.  
  - `metric=lb_backend_idle_close_total` 이 급증하면 gateway-backend 연결이 비정상임을 의미하므로 Alertmanager에서 5분 누적 5건 이상일 때 경고를 보낸다.
- **Postgres**  
  - write-behind 워커가 실패하면 DLQ에 적재하고 `metric=wb_fail_total` 이 증가한다. 1분 동안 0이 아닌 상태가 지속되면 경고 알람을 발송한다.  
  - 서버는 snapshot 조회 실패 시 Redis 캐시만으로 응답하되, 관리자에게 “DB fallback 사용 중” 로그를 남긴다.

## 2. 게이트웨이·로드밸런서
- gRPC 스트림이 끊기면 게이트웨이는 재연결을 시도하고, 3회 연속 실패 시 `SESSION_MOVED` 알림을 클라이언트에 전달한다.  
- 로드밸런서는 backend 연결에 실패하면 `mark_backend_failure()` 를 호출해 링에서 일시적으로 제외한다.

## 3. 알림 규칙 예시
| 지표 | 조건 | 조치 |
| --- | --- | --- |
| `sum(increase(lb_backend_idle_close_total[5m]))` | 5 이상 | gateway ↔ backend 포트 확인, Redis sticky 상태 확인 |
| `aiops_pg_connect_error_total` | 3분 연속 증가 | RDS 상태 점검 후 필요 시 failover |
| `chat_subscribe_last_lag_ms` | p95 > 200ms (5분) | Pub/Sub 채널 점검, gateway 로그 확인 |
| `wb_pending` | 500 이상 (5분) | DLQ 확인, 배포 중단 |

## 4. Runbook 연계
1. Alertmanager → Slack/Webhook  
2. on-call 엔지니어는 `docs/ops/runbook.md` 의 체크리스트에 따라 조치  
3. 조치 결과와 재발 방지를 issue tracker에 기록

## 5. 참고
- `docs/ops/observability.md` – 지표/로그 수집  
- `docs/ops/runbook.md` – 장애 대응 절차  
- `docs/ops/dlq-retry.md` – DLQ 처리 흐름
