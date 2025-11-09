# Fallback & Alerts (상세)

서비스 중 장애가 발생했을 때 어떤 fallback 을 사용할지, 어떤 알람을 걸어야 하는지 정리한다.

## 1. 시나리오별 대응
| 시나리오 | 현상 | 즉시 조치 | Fallback |
| --- | --- | --- | --- |
| Redis 장애 | Sticky session 미작동, Pub/Sub 지연 | ① Redis ping ② Sentinel/ElastiCache 상태 확인 | Load Balancer 가 Consistent Hash 만 사용하도록 `LB_REDIS_URI` 비워 재배포 (임시) |
| PostgreSQL 장애 | write-behind 실패, 스냅샷 DB 조회 실패 | ① RDS failover ② `wb_worker` 중단 | Redis recent cache 만으로 snapshot 제공, 채팅은 운영자가 `write_behind_enabled=0` 으로 전환 |
| Gateway-LB gRPC 끊김 | 모든 클라이언트가 재연결 반복 | ① LB gRPC 로그 확인 ② security group/로드밸런서 상태 | Gateway 는 LB 엔드포인트 재설정, 필요 시 LB rolling restart |
| Backend CPU 100% | `chat_session_active=0`, `lb_backend_idle_close_total` 급증 | ① 병목 thread dump ② HPA scale out | `LB_BACKEND_FAILURE_THRESHOLD` 를 낮춰 문제 노드를 빠르게 격리 |

## 2. Alert Rule 예시
| PromQL | 임계치 | 알림 메시지 |
| --- | --- | --- |
| `sum(increase(lb_backend_idle_close_total[5m]))` | > 5 | "LB idle close spike" |
| `chat_subscribe_last_lag_ms{quantile="0.95"}` | > 200ms (5m) | "Redis Pub/Sub lag" |
| `wb_pending` | > 500 (5m) | "write-behind backlog" |
| `sum(increase(wb_dlq_replay_dead_total[5m]))` | > 0 | "DLQ dead events" |
| `sum(rate(chat_dispatch_exception_total[1m]))` | > 1/s | "Server dispatch exceptions" |

AlertManager → Slack → On-call 순으로 전달하고, runbook 절차에 따라 대응한다.

## 3. Fallback 절차 세부
### 3.1 Redis sticky 비활성화
1. Helm values 에 `LB_REDIS_URI=""`, `LB_SESSION_TTL=0`
2. `helm upgrade load-balancer ...` 배포
3. `lb_backend_idle_close_total` 와 `chat_session_active` 추이 모니터링
4. Redis 복구 후 다시 설정

### 3.2 write-behind 임시 중단
1. `WRITE_BEHIND_ENABLED=0` 로 server_app 재배포 (flush 중지)
2. `wb_worker` Deployment scale=0
3. Redis Streams backlog 확인(삭제 금지)
4. DB 복구 후 역순으로 되돌림

### 3.3 Gateway gRPC 재연결
1. Gateway pods 에서 `kubectl logs` 로 오류 확인
2. LB endpoint 가 변경됐으면 ConfigMap 업데이트
3. `scripts/smoke_gateway_lb.ps1` (추후 TODO) 로 health 여부 확인

## 4. 로그 규칙
- 모든 fallback/알람 관련 조치는 `metric=*` 형식과 `action=fallback-{type}` 태그를 포함한 INFO 로그로 남긴다.
- 예: `metric=lb_backend_idle_close_total value=1 action=fallback-disable-sticky`.

## 5. 참고 문서
- `docs/ops/runbook.md`
- `docs/ops/deployment.md`
- `docs/ops/observability.md`
