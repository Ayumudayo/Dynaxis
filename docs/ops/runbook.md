# 운영 체크리스트 (Runbook)

## 1. 배포 전 점검
- [ ] `.env` / Secrets 값 확인 (`GATEWAY_ID`, `DB_URI`, `REDIS_URI`, `METRICS_PORT`)  
- [ ] `./migrations_runner status` 로 스키마 최신 여부 확인  
- [ ] `/metrics` HTTP 200, 주요 지표 존재 확인  
- [ ] Redis/PubSub 연결 확인 (`redis-cli PING`, `XINFO`)  
- [ ] 로그 파이프라인에서 `metric=*` 형식이 수집되는지 검증

## 2. 배포 절차 (Helm 예시)
1. `helm upgrade --install server-app charts/server-app -f values/prod.yaml`  
2. `kubectl rollout status deploy/server-app`  
3. Smoke 테스트: devclient로 `/rooms`, 기본 채팅 시나리오  
4. Grafana “Active Sessions”, “LB Idle Close Rate” 를 5분간 모니터링

## 3. 알람 대응
| Alert | 점검 순서 |
| --- | --- |
| `sum(increase(lb_backend_idle_close_total[5m])) > 5` | ① gateway ↔ backend 포트 ② Redis sticky 상태 ③ backend 로그 |
| `chat_subscribe_last_lag_ms p95 > 200ms` | ① Redis latency ② 게이트웨이 CPU ③ Pub/Sub 채널 누락 |
| `wb_pending > 500 (5m)` | ① DB 상태 ② 워커 로그(`metric=wb_fail_total`) ③ DLQ 재처리 |
| `chat_session_active == 0` | ① HPA 축소 여부 ② Load Balancer 링 구성을 확인 |

## 4. 장애 복구 흐름
1. AlertManager → Slack → 온콜 담당자  
2. 이 문서의 체크리스트 따라 원인 파악  
3. 필요한 경우 fallback 실행 (`docs/ops/fallback-and-alerts.md`)  
4. 해결 후 Incident Report 작성 (원인/조치/재발 방지)

## 5. 참조 링크
- `docs/ops/observability.md` – 메트릭/대시보드  
- `docs/ops/distributed_routing_draft.md` – 라우팅 구조  
- `docs/ops/dlq-retry.md` – DLQ 복구  
- `docs/db/write-behind.md` – write-behind 설계
