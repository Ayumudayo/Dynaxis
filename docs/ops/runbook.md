# 운영 체크리스트

이 문서는 배포 전후 점검, 알람 대응, 대표 장애 시나리오, 스모크 테스트 순서를 한곳에 모은 운영 체크리스트다. 설계 문서가 "왜 이런 구조인가"를 설명한다면, 이 문서는 "지금 무엇을 어떤 순서로 해야 하는가"를 빠르게 찾게 해 주는 데 목적이 있다.

운영 문서에서 순서가 중요한 이유는 장애나 배포 중에는 판단 여유가 줄어들기 때문이다. 따라서 이 문서는 상세 설명보다 먼저 실행 순서를 고정해 두는 데 집중한다.

## 1. 배포 전후 공통 점검

1. `.env`와 Secrets를 확인한다. (`DB_URI`, `REDIS_URI`, `METRICS_PORT`)
2. `/metrics`가 200 OK를 반환하는지, `runtime_build_info` 라벨이 기대 배포 버전인지 확인한다. 예: `curl http://svc:9090/metrics`
3. `migrations_runner status`에서 pending이 없는지 확인한다.
4. Grafana에서 "Active Sessions"가 정상이고, write-behind backlog와 에러 지표가 이상 없는지 확인한다.
5. Alertmanager silence 설정 여부를 확인한다. 배포 중에는 silence를 켜고, 완료 뒤에는 해제한다.

이 다섯 단계를 같이 보는 이유는 "배포는 끝났는데 관측과 마이그레이션이 어긋난 상태"를 막기 위해서다. 코드만 올라간 상태를 성공으로 보면 안 된다.

## 2. 알람 대응 매트릭스

| 알람 | 증상 | 조치 순서 |
| --- | --- | --- |
| Redis Lag | `chat_subscribe_last_lag_ms p95 > 200ms` | (1) Redis INFO latency (2) Pub/Sub 사용량 (3) 게이트웨이 로그 |
| Write-behind backlog | `wb_pending > 500` | (1) DB 세션 확인 (2) `wb_worker` 로그 (3) DLQ 상태 |
| Gateway backend circuit open | `gateway_backend_circuit_open==1` 지속 | (1) `server_app` readiness 확인 (2) `gateway_backend_*` 실패 지표 확인 (3) `GATEWAY_BACKEND_CIRCUIT_*` 임계치 점검 |
| Gateway ingress rate-limit | `gateway_ingress_reject_rate_limit_total` 급증 | (1) 접속 폭주나 공격 source 확인 (2) `GATEWAY_INGRESS_*` 임계치 조정 (3) gateway replica 확장 |
| wb flush retry exhausted | `wb_flush_retry_exhausted_total` 증가 | (1) DB 가용성과 락 상태 점검 (2) `WB_RETRY_*`, `WB_DB_RECONNECT_*` 조정 (3) reclaim backlog 증가 여부 확인 |
| TLS cert expiry (30d) | `TLSCertificateExpiringIn30Days` 발생 | (1) 갱신 일정 확정 (2) 대상 인증서와 SAN 목록 점검 (3) 스테이지 롤링 계획 수립 |
| TLS cert expiry (14d) | `TLSCertificateExpiringIn14Days` 발생 | (1) 스테이지 갱신 리허설 (2) mTLS 체인 검증 (3) 본 배포 승인 |
| TLS cert expiry (7d) | `TLSCertificateExpiringIn7Days` 발생 | (1) 즉시 인증서 교체 (2) legacy 예외 listener 포함 전수 반영 (3) 만료 임계치 해소 확인 |
| Dispatch Exception | `chat_dispatch_exception_total` 급증 | (1) `server_app` 로그 (2) 최근 배포 롤백 |
| UDP bind abuse | `gateway_udp_bind_rate_limit_reject_total` 증가 + `gateway_udp_bind_block_total` 증가 | (1) 공격/오탐 source IP 확인 (2) `GATEWAY_UDP_BIND_FAIL_*`, `GATEWAY_UDP_BIND_BLOCK_MS` 재검토 (3) 필요 시 UDP ingress 제한 |
| UDP quality degradation | `GatewayUdpEstimatedLossHigh` 또는 `GatewayUdpJitterHigh` 발생 | (1) `gateway-udp-quality` 대시보드에서 loss, jitter, replay 분해 (2) 네트워크 구간 확인 (3) 필요 시 UDP 대상 opcode 축소 또는 TCP fallback |
| RUDP handshake/retransmit/fallback 이상 | `RudpHandshakeFailureSpike`, `RudpRetransmitRatioHigh`, `RudpFallbackSpike` 발생 | (1) canary 비율 즉시 0으로 축소 (2) `GATEWAY_RUDP_ENABLE=0`으로 신규 세션 RUDP 차단 (3) TCP KPI 복귀 확인 뒤 원인 분석 |

이 표를 두는 이유는 on-call 담당자가 알람 이름만 보고도 바로 첫 세 단계를 시작할 수 있어야 하기 때문이다. 알람 설명과 조치 순서가 분리돼 있으면 실제 대응 속도가 느려진다.

## 3. 장애 시나리오

### 3.1 Redis 장애

1. `redis-cli -h <endpoint> PING`
2. 실패 시 ElastiCache 상태와 보안그룹 확인
3. 임시 조치: HAProxy에서 일부 Gateway를 drain하거나 트래픽을 제한하고, Redis 복구를 우선 수행
4. 복구 뒤 Redis TTL 상태를 확인하고 stale sticky를 제거

Redis를 먼저 보는 이유는 sticky, registry, Pub/Sub가 함께 영향을 받기 때문이다. 증상이 여러 곳에서 보일수록 공용 의존성을 먼저 의심하는 편이 빠르다.

### 3.2 PostgreSQL 장애

1. `psql`로 health 체크
2. RDS failover 또는 read replica 승격
3. `WRITE_BEHIND_ENABLED=0`으로 서버 재배포 -> 메모리 snapshot 모드
4. DB 복구 뒤 write-behind 재개, DLQ 모니터링

DB 장애 시 write-behind를 억지로 유지하지 않는 이유는 backlog와 재시도 실패가 동시에 쌓이면 나중에 복구 경로가 더 복잡해지기 때문이다.

### 3.3 HAProxy ↔ Gateway 연결 문제

1. HAProxy 백엔드 상태(다운, 체크 실패) 확인
2. Gateway pod 로그 확인 (listen 실패, Redis 연결 실패 등)
3. 필요 시 Gateway를 순차 재시작한다.
   HAProxy 백엔드에서 제외 -> 재기동 -> 복귀

이 순서를 지키는 이유는 연결 문제처럼 보여도 edge 설정, gateway 프로세스, Redis 의존성 중 어디서 막혔는지 다를 수 있기 때문이다.

### 3.4 UDP bind 반복 실패 또는 차단 급증

1. `gateway_udp_bind_reject_total`, `gateway_udp_bind_rate_limit_reject_total`, `gateway_udp_bind_block_total` 증가 시점을 확인
2. source IP와 포트 분포를 확인해 단일 공격원인지, NAT 뒤 정상 사용자 다수인지 구분
3. 오탐이면 `GATEWAY_UDP_BIND_FAIL_WINDOW_MS`, `GATEWAY_UDP_BIND_FAIL_LIMIT`, `GATEWAY_UDP_BIND_BLOCK_MS`를 완화
4. 공격이면 edge(LB/WAF)에서 먼저 차단하고, 필요 시 `GATEWAY_UDP_LISTEN` 비활성으로 TCP 전용 경로 복귀

공격과 오탐을 먼저 구분해야 하는 이유는 대응이 정반대이기 때문이다. 정상 사용자를 공격으로 오인하면 품질이 나빠지고, 실제 공격을 오탐으로 보면 차단이 늦어진다.

### 3.5 UDP canary와 rollback 리허설

1. canary 오픈: `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-canary.example`
2. `gateway_udp_enabled` 상태 확인 (`gateway-1=1`, `gateway-2=0`)
3. rollback: `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-rollback.example`
4. rollback 뒤 `gateway_udp_enabled`가 양 gateway에서 0인지 확인하고 `python tests/python/verify_pong.py`로 TCP 스모크 검증

리허설을 별도로 적는 이유는 실제 장애 때 처음 해 보는 절차가 되지 않게 하기 위해서다. rollout과 rollback은 둘 다 반복 숙련이 필요하다.

### 3.6 RUDP canary와 rollback

1. 전제 확인: 운영 런타임 기본값은 `GATEWAY_RUDP_ENABLE=0`이며, canary 전개 시에도 allowlist 기반으로 신규 세션만 점진 적용
2. canary 오픈: `GATEWAY_RUDP_ENABLE=1`, `GATEWAY_RUDP_CANARY_PERCENT=<소량>`, `GATEWAY_RUDP_OPCODE_ALLOWLIST=<opcode,...>` 설정 뒤 신규 세션에서만 관찰
3. 모니터링: `RudpHandshakeFailureSpike`(실패율 >20%), `RudpRetransmitRatioHigh`(재전송비율 >15%), `RudpFallbackSpike`(fallback >0.1/s)와 원인 지표(`core_runtime_rudp_*`, `gateway_rudp_*`)를 함께 확인
4. 이상 시 즉시 롤백
   - `GATEWAY_RUDP_CANARY_PERCENT=0`
   - `GATEWAY_RUDP_ENABLE=0`
   - TCP 전용 스모크(`python tests/python/verify_pong.py`)와 핵심 KPI 복귀 확인

RUDP는 부분 rollout이 가능한 대신, 품질 저하가 느리게 퍼질 수 있다. 그래서 신규 세션만 제한적으로 열고, 이상 시 즉시 다시 0으로 내리는 규칙이 중요하다.

## 4. 스모크 테스트 절차

1. `client_gui` 또는 동등한 e2e 클라이언트로 `/login runbook` -> `/join lobby` -> `/chat runbook-check`
2. `/refresh`로 snapshot 정상 반환 여부 확인
3. `wb_emit`으로 write-behind 이벤트 발행 -> `wb_worker` 로그 확인
4. Grafana 대시보드 스크린샷 저장 (배포 뒤 5분)

스모크 절차를 이렇게 묶는 이유는 로그인, snapshot, write-behind, 관측 지점이 함께 살아 있어야 "정상 서비스"라고 볼 수 있기 때문이다.

## 5. 장애 보고서 작성 템플릿

```text
- 날짜/시간:
- 탐지 경로 (알림, 고객, 내부 모니터링):
- 영향 범위:
- 원인 요약:
- 조치 내용:
- 재발 방지:
```

모든 주요 장애는 24시간 안에 장애 보고서를 작성한다.

사후 문서를 강제하는 이유는 동일 장애가 반복될 때 "예전에 어떻게 대응했는가"가 바로 남아 있어야 하기 때문이다. 장애를 복구만 하고 기록하지 않으면 같은 비용을 다시 치르게 된다.

## 6. 참고

- `docs/ops/fallback-and-alerts.md`
- `docs/ops/observability.md`
- `docs/ops/udp-rollout-rollback.md`
