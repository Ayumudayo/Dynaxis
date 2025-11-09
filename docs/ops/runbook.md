# 운영 체크리스트 (Runbook – Expanded)

## 1. 배포 전/후 공통 점검
1. `.env` / Secrets 확인 (`DB_URI`, `REDIS_URI`, `METRICS_PORT`, `JWT_SECRET`)
2. `migrations_runner status` → pending 없음 확인
3. `/metrics` 200 OK (`curl http://svc:9090/metrics`)
4. Grafana "Active Sessions" 정상, `lb_backend_idle_close_total` 증가 없음
5. Alertmanager silence 설정 여부 확인 (배포 중엔 silence ON, 완료 후 OFF)

## 2. 알람 대응 매트릭스
| 알람 | 증상 | 조치 순서 |
| --- | --- | --- |
| LB Idle Spike | `sum(increase(lb_backend_idle_close_total[5m])) > 5` | (1) backend pod CPU 확인 (2) Redis latency (3) `LB_BACKEND_IDLE_TIMEOUT` 조정 |
| Redis Lag | `chat_subscribe_last_lag_ms p95 > 200ms` | (1) Redis INFO latency (2) Pub/Sub 사용량 (3) 게이트웨이 로그 |
| Write-behind backlog | `wb_pending > 500` | (1) DB 세션 확인 (2) `wb_worker` 로그 (3) DLQ 상태 |
| Dispatch Exception | `chat_dispatch_exception_total` 급증 | (1) server_app 로그 (2) 최근 배포 롤백 |

## 3. 장애 시나리오
### 3.1 Redis 장애
1. `redis-cli -h <endpoint> PING`
2. 실패 시 ElastiCache 상태/보안그룹 확인
3. 임시 조치: `LB_REDIS_URI` 비우고 Load Balancer 재배포 → sticky off
4. 복구 후 Redis TTL 상태 확인, stale sticky 제거

### 3.2 PostgreSQL 장애
1. `psql` 로 health 체크
2. RDS failover 또는 read replica 승격
3. `WRITE_BEHIND_ENABLED=0` 로 서버 재배포 → 메모리 snapshot 모드
4. DB 복구 후 write-behind 재개, DLQ 모니터링

### 3.3 Gateway-LB 연결 문제
1. Gateway pod 로그 `metric=gateway_lb_reconnect_total` (TODO) 확인
2. gRPC endpoint/Secret 값이 최신인지 확인
3. 필요 시 Gateway와 LB 를 순서대로 재시작 (LB → Gateway)

## 4. Smoke 테스트 절차
1. devclient 실행 → `/login runbook` → `/join lobby` → `/chat runbook-check`
2. `/refresh` 로 snapshot 정상 반환 여부 확인
3. `wb_emit` 로 write-behind 이벤트 발행 → `wb_worker` 로그 확인
4. Grafana 대시보드 스크린샷 저장 (배포 후 5분)

## 5. Incident Report 작성 템플릿
```
- 날짜/시간:
- 탐지 경로 (Alert, 고객, 내부 모니터링):
- 영향 범위:
- 원인 요약:
- 조치 내용:
- 재발 방지:
```
모든 주요 장애는 24시간 내 Incident Report 를 작성한다.

## 6. 참고
- `docs/ops/fallback-and-alerts.md`
- `docs/ops/deployment.md`
- `docs/ops/observability.md`
