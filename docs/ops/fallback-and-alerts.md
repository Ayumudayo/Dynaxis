# 장애 대응과 알림

서비스 운영 중 장애가 발생했을 때 어떤 fallback을 사용할지, 어떤 알림을 설정해야 하는지 정리한다. 이 문서의 목적은 알람 이름만 나열하는 데 있지 않다. "어떤 증상이 오면 어떤 계층을 먼저 의심해야 하는가", "즉시 완화와 근본 복구를 어떻게 분리할 것인가"를 같은 언어로 맞추는 것이 더 중요하다.

이런 문서가 필요한 이유는 운영 장애가 보통 한 지표만으로 설명되지 않기 때문이다. 예를 들어 Redis 장애는 sticky routing, instance registry, Pub/Sub, snapshot fallback까지 연쇄적으로 영향을 준다. 따라서 알림과 fallback을 같은 문서 안에서 같이 봐야 판단이 빨라진다.

## 1. 시나리오별 대응

| 시나리오 | 현상 | 즉시 조치 | fallback |
| --- | --- | --- | --- |
| Redis 장애 | sticky session, Instance Registry 미작동, Pub/Sub 지연 | ① Redis ping ② Sentinel/ElastiCache 상태 확인 | 현재 스택은 Redis 의존도가 높다. Redis 복구가 우선이며, 복구 전에는 신규 라우팅과 sticky 품질이 저하될 수 있다. |
| PostgreSQL 장애 | write-behind 실패, snapshot DB 조회 실패 | ① RDS failover ② `wb_worker` 중단 | Redis recent cache만으로 snapshot 제공, 채팅은 운영자가 `write_behind_enabled=0`으로 전환 |
| HAProxy ↔ Gateway 연결 문제 | 모든 클라이언트 접속 실패 또는 재연결 반복 | ① HAProxy 백엔드 상태 ② Gateway listen과 로그 ③ 네트워크/보안그룹 | 임시로 특정 Gateway로 직접 접속해 디버깅하거나, 문제가 있는 Gateway를 HAProxy 백엔드에서 제외 |
| backend CPU 100% | 지연 급증, 접속 실패, 드롭 | ① 병목 thread dump ② HPA scale out | 문제 노드를 트래픽에서 격리(예: Instance Registry TTL 만료 유도, 배포 롤링) |

이 표를 먼저 두는 이유는 장애 문서를 읽는 사람이 보통 "무슨 설계인가"보다 "지금 이 증상에서 어디부터 볼 것인가"를 먼저 찾기 때문이다.

## 2. 알림 규칙 예시

| PromQL | 임계치 | 알림 메시지 |
| --- | --- | --- |
| `chat_subscribe_last_lag_ms{quantile="0.95"}` | > 200ms (5m) | "Redis Pub/Sub lag" |
| `wb_pending` | > 500 (5m) | "write-behind backlog" |
| `sum(increase(wb_dlq_replay_dead_total[5m]))` | > 0 | "DLQ dead events" |
| `sum(rate(chat_dispatch_exception_total[1m]))` | > 1/s | "Server dispatch exceptions" |
| `sum(rate(gateway_udp_bind_rate_limit_reject_total[5m]))` | > 1/s (10m) | "UDP bind abuse spike" |
| `sum(rate(gateway_udp_loss_estimated_total[5m])) / clamp_min(sum(rate(gateway_udp_forward_total[5m])), 1)` | > 0.05 (10m) | "UDP estimated loss high" |
| `sum(rate(gateway_udp_replay_drop_total[5m]))` | > 2/s (10m) | "UDP replay/reorder drops high" |
| `max_over_time(gateway_udp_jitter_ms_last[10m])` | > 150ms (10m) | "UDP jitter high" |
| `sum(rate(core_runtime_rudp_handshake_total{result="fail"}[5m])) / clamp_min(sum(rate(core_runtime_rudp_handshake_total[5m])), 1)` | > 0.20 + handshake > 0.1/s (10m) | "RUDP handshake failure ratio elevated" |
| `sum(rate(core_runtime_rudp_retransmit_total[5m])) / clamp_min(sum(rate(gateway_rudp_inner_forward_total[5m])), 1)` | > 0.15 + forward > 1/s (10m) | "RUDP retransmit ratio high" |
| `sum(rate(gateway_rudp_fallback_total[5m]))` | > 0.1/s (10m) | "RUDP fallback spike" |
| `(probe_ssl_earliest_cert_expiry - time()) <= 30d and > 14d` | 4h 지속 | "TLS cert expires in <= 30 days" |
| `(probe_ssl_earliest_cert_expiry - time()) <= 14d and > 7d` | 1h 지속 | "TLS cert expires in <= 14 days" |
| `(probe_ssl_earliest_cert_expiry - time()) <= 7d` | 5m 지속 | "TLS cert expires in <= 7 days" |

AlertManager -> Slack -> On-call 순으로 전달하고, runbook 절차에 따라 대응한다.

알림을 이 정도로 구체화하는 이유는 "경보는 왔는데 무엇을 뜻하는지 모르겠다"는 상태를 줄이기 위해서다. 임계치와 메시지가 함께 있어야 on-call 담당자가 바로 다음 문서로 넘어갈 수 있다.

## 3. fallback 절차 상세

### 3.1 Redis 장애 대응

1. Redis 복구를 최우선으로 둔다. sticky, Instance Registry, Pub/Sub 경로가 모두 영향을 받기 때문이다.
2. Redis 복구 전에는 신규 라우팅이 불안정할 수 있으므로, 필요하면 Gateway 트래픽을 제한하거나 HAProxy 백엔드에서 일부를 제외한 뒤 점진적으로 복구한다.

Redis를 먼저 복구하는 이유는 이 계층이 여러 기능의 공용 기반이기 때문이다. 다른 증상이 보여도 실제 근원은 Redis일 수 있다.

### 3.2 write-behind 임시 중단

1. `WRITE_BEHIND_ENABLED=0`으로 `server_app`을 재배포해 flush를 멈춘다.
2. `wb_worker` deployment를 scale 0으로 내린다.
3. Redis Streams backlog를 확인한다. 삭제는 금지한다.
4. DB 복구 뒤 역순으로 되돌린다.

이 절차를 따로 두는 이유는 DB 장애 시 write-behind 경로를 무리하게 계속 밀어넣으면 backlog 해석과 재처리가 더 어려워지기 때문이다. 먼저 멈추고, backlog를 보존하고, 복구 뒤 다시 읽게 하는 편이 낫다.

### 3.3 HAProxy ↔ Gateway 연결 점검

1. HAProxy 로그와 상태 페이지에서 backend down 여부를 확인한다.
2. Gateway pod에서 `kubectl logs`로 오류를 확인한다.
3. HAProxy 백엔드와 health check 설정이 최신인지 확인한다.

이 순서를 권장하는 이유는 edge에서 끊긴 것처럼 보여도 실제 원인이 Gateway listen 실패나 Redis 의존성 실패일 수 있기 때문이다. LB와 gateway를 같이 봐야 한다.

### 3.4 RUDP canary 이상 대응

1. `RudpHandshakeFailureSpike`가 발생하면 `GATEWAY_RUDP_CANARY_PERCENT=0`으로 즉시 축소한다.
2. `RudpRetransmitRatioHigh`가 지속되면 네트워크 경로와 `GATEWAY_RUDP_OPCODE_ALLOWLIST`를 점검하거나 축소한다.
3. `RudpFallbackSpike`가 발생하면 `GATEWAY_RUDP_ENABLE=0`으로 신규 세션 RUDP를 즉시 차단한다.
4. `python tests/python/verify_pong.py`로 TCP 스모크를 수행하고 핵심 KPI 복귀를 확인한다.

RUDP 쪽은 "부분 비활성 -> 전체 비활성" 순으로 좁혀 가는 것이 중요하다. 모든 문제를 곧바로 전체 shutdown으로 대응하면 원인 분석이 어려워지고, 반대로 너무 오래 canary를 유지하면 품질 저하가 사용자에게 퍼질 수 있다.

## 4. 로그 규칙

- 모든 fallback과 알림 관련 조치는 `metric=*` 형식과 `action=fallback-{type}` 태그를 포함한 INFO 로그로 남긴다.
- 예: `metric=gateway_backend_connect_fail_total value=1 action=fallback-drain-gateway`

이 규칙이 필요한 이유는 사후 분석에서 "무슨 지표를 보고 어떤 대응을 했는가"를 다시 맞춰 볼 수 있어야 하기 때문이다. 대응 이력이 로그에 구조적으로 남지 않으면 알람과 사람의 조치가 분리된다.

## 5. 참고 문서

- `docs/ops/runbook.md`
- `docs/ops/observability.md`
- `docs/ops/udp-rollout-rollback.md`
