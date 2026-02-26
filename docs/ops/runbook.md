# 운영 체크리스트 (확장판)

## 1. 배포 전/후 공통 점검
1. `.env` / Secrets 확인 (`DB_URI`, `REDIS_URI`, `METRICS_PORT`)
2. `/metrics` 200 OK + `knights_build_info` 라벨로 배포 버전 확인 (`curl http://svc:9090/metrics`)
3. `migrations_runner status` → pending 없음 확인
4. Grafana "Active Sessions" 정상, write-behind backlog/에러 지표 이상 없음
5. Alertmanager silence 설정 여부 확인 (배포 중엔 silence ON, 완료 후 OFF)

## 2. 알람 대응 매트릭스
| 알람 | 증상 | 조치 순서 |
| --- | --- | --- |
| Redis Lag | `chat_subscribe_last_lag_ms p95 > 200ms` | (1) Redis INFO latency (2) Pub/Sub 사용량 (3) 게이트웨이 로그 |
| Write-behind backlog | `wb_pending > 500` | (1) DB 세션 확인 (2) `wb_worker` 로그 (3) DLQ 상태 |
| Dispatch Exception | `chat_dispatch_exception_total` 급증 | (1) server_app 로그 (2) 최근 배포 롤백 |
| UDP bind abuse | `gateway_udp_bind_rate_limit_reject_total` 증가 + `gateway_udp_bind_block_total` 증가 | (1) 공격/오탐 source IP 확인 (2) `GATEWAY_UDP_BIND_FAIL_*`/`GATEWAY_UDP_BIND_BLOCK_MS` 재검토 (3) 필요 시 임시로 UDP ingress 제한 |
| UDP quality degradation | `GatewayUdpEstimatedLossHigh` 또는 `GatewayUdpJitterHigh` 발생 | (1) `gateway-udp-quality` 대시보드에서 loss/jitter/replay 분해 (2) 네트워크 구간 확인 (3) 필요 시 UDP 대상 opcode 축소 또는 TCP fallback |

## 3. 장애 시나리오
### 3.1 Redis 장애
1. `redis-cli -h <endpoint> PING`
2. 실패 시 ElastiCache 상태/보안그룹 확인
3. 임시 조치: HAProxy에서 일부 Gateway를 drain(백엔드 제외)하거나 트래픽을 제한하고, Redis 복구를 우선 수행
4. 복구 후 Redis TTL 상태 확인, stale sticky 제거

### 3.2 PostgreSQL 장애
1. `psql` 로 health 체크
2. RDS failover 또는 read replica 승격
3. `WRITE_BEHIND_ENABLED=0` 로 서버 재배포 → 메모리 snapshot 모드
4. DB 복구 후 write-behind 재개, DLQ 모니터링

### 3.3 HAProxy↔Gateway 연결 문제
1. HAProxy 백엔드 상태(다운/체크 실패) 확인
2. Gateway pod 로그 확인 (listen 실패, Redis 연결 실패 등)
3. 필요 시 Gateway를 순차 재시작(HAProxy 백엔드에서 제외 → 재기동 → 복귀)

### 3.4 UDP bind 반복 실패/차단 급증
1. `gateway_udp_bind_reject_total`, `gateway_udp_bind_rate_limit_reject_total`, `gateway_udp_bind_block_total`의 증가 시점 확인
2. source IP/포트 분포를 확인해 단일 공격원인지, NAT 뒤 정상 사용자 다수인지 구분
3. 오탐이면 `GATEWAY_UDP_BIND_FAIL_WINDOW_MS`, `GATEWAY_UDP_BIND_FAIL_LIMIT`, `GATEWAY_UDP_BIND_BLOCK_MS`를 완화
4. 공격이면 edge(LB/WAF)에서 선차단하고, 필요 시 `GATEWAY_UDP_LISTEN` 비활성으로 TCP-only 복귀

### 3.5 UDP canary/rollback 리허설
1. canary 오픈: `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-canary.example`
2. `gateway_udp_enabled` 상태 확인(gateway-1=1, gateway-2=0)
3. rollback: `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-rollback.example`
4. rollback 후 `gateway_udp_enabled`가 양 gateway에서 0인지 확인하고 `python tests/python/verify_pong.py`로 TCP smoke 검증

## 4. 스모크 테스트 절차
1. devclient 실행 → `/login runbook` → `/join lobby` → `/chat runbook-check`
2. `/refresh` 로 snapshot 정상 반환 여부 확인
3. `wb_emit` 로 write-behind 이벤트 발행 → `wb_worker` 로그 확인
4. Grafana 대시보드 스크린샷 저장 (배포 후 5분)

## 5. 장애 보고서 작성 템플릿
```
- 날짜/시간:
- 탐지 경로 (알림, 고객, 내부 모니터링):
- 영향 범위:
- 원인 요약:
- 조치 내용:
- 재발 방지:
```
모든 주요 장애는 24시간 내 장애 보고서를 작성한다.

## 6. 참고
- `docs/ops/fallback-and-alerts.md`
- `docs/ops/deployment.md`
- `docs/ops/observability.md`
- `docs/ops/udp-rollout-rollback.md`
