# 장애 대응(Fallback) 및 알림(Alerts) (상세)

서비스 중 장애가 발생했을 때 어떤 fallback을 사용할지, 어떤 알림을 설정해야 하는지 정리한다.

## 1. 시나리오별 대응
| 시나리오 | 현상 | 즉시 조치 | Fallback |
| --- | --- | --- | --- |
| Redis 장애 | Sticky session/Instance Registry 미작동, Pub/Sub 지연 | ① Redis ping ② Sentinel/ElastiCache 상태 확인 | 현재 스택은 Redis 의존도가 높다. Redis 복구가 우선이며, 복구 전에는 신규 라우팅/스티키 품질이 저하될 수 있다. |
| PostgreSQL 장애 | write-behind 실패, 스냅샷 DB 조회 실패 | ① RDS failover ② `wb_worker` 중단 | Redis recent cache 만으로 snapshot 제공, 채팅은 운영자가 `write_behind_enabled=0` 으로 전환 |
| HAProxy↔Gateway 연결 문제 | 모든 클라이언트 접속 실패/재연결 반복 | ① HAProxy 백엔드 상태 ② Gateway listen/로그 ③ 네트워크/보안그룹 | 임시로 특정 Gateway로 직접 접속(디버깅)하거나, 문제가 있는 Gateway를 HAProxy 백엔드에서 제외 |
| Backend CPU 100% | 지연 급증/접속 실패/드랍 | ① 병목 thread dump ② HPA scale out | 문제 노드를 트래픽에서 격리(예: Instance Registry TTL 만료 유도, 배포 롤링) |

## 2. Alert Rule 예시
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

AlertManager → Slack → On-call 순으로 전달하고, runbook 절차에 따라 대응한다.

## 3. Fallback 절차 상세
### 3.1 Redis 장애 대응
1. Redis 복구가 최우선(Sticky/Instance Registry/PubSub 경로 모두 영향)
2. Redis 복구 전에는 신규 라우팅이 불안정할 수 있으므로, 필요 시 Gateway 트래픽을 제한하거나(HAProxy 백엔드 제외) 점진적으로 복구

### 3.2 write-behind 임시 중단
1. `WRITE_BEHIND_ENABLED=0` 로 server_app 재배포 (flush 중지)
2. `wb_worker` Deployment scale=0
3. Redis Streams backlog 확인(삭제 금지)
4. DB 복구 후 역순으로 되돌림

### 3.3 HAProxy↔Gateway 연결 점검
1. HAProxy 로그/상태 페이지에서 backend 다운 여부 확인
2. Gateway pods 에서 `kubectl logs` 로 오류 확인
3. HAProxy 백엔드/헬스체크 설정이 최신인지 확인

### 3.4 RUDP canary 이상 대응
1. `RudpHandshakeFailureSpike` 발생 시 `GATEWAY_RUDP_CANARY_PERCENT=0`으로 즉시 축소
2. `RudpRetransmitRatioHigh`가 지속되면 네트워크 경로와 `GATEWAY_RUDP_OPCODE_ALLOWLIST`를 점검/축소
3. `RudpFallbackSpike` 발생 시 `GATEWAY_RUDP_ENABLE=0`으로 신규 세션 RUDP를 즉시 차단
4. `python tests/python/verify_pong.py`로 TCP smoke를 수행하고 핵심 KPI 복귀를 확인

## 4. 로그 규칙
- 모든 fallback/알림 관련 조치는 `metric=*` 형식과 `action=fallback-{type}` 태그를 포함한 INFO 로그로 남긴다.
- 예: `metric=gateway_backend_connect_fail_total value=1 action=fallback-drain-gateway`.

## 5. 참고 문서
- `docs/ops/runbook.md`
- `docs/ops/observability.md`
- `docs/ops/udp-rollout-rollback.md`
