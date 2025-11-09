# Gateway & Load Balancer 운영 가이드 (상세)

Gateway 와 Load Balancer 를 배포/운영할 때 필요한 모든 정보를 정리했다.

## 1. 전체 흐름
```
Client --TCP--> Gateway --gRPC Stream--> Load Balancer --TCP--> server_app
                                │
                                ├─ Redis (SessionDirectory, Instance Registry)
                                └─ Prometheus (로그 지표)
```
1. Gateway 는 TCP 세션을 수락하고 인증 후 `open_lb_session()` 을 호출
2. LB는 Consistent Hash + Sticky Session 으로 backend를 선택해 TCP 연결
3. 데이터는 gRPC + TCP 프락시로 전달, heartbeat 는 `ROUTE_KIND_HEARTBEAT`
4. `LB_BACKEND_IDLE_TIMEOUT` 과 health check 로 비정상 연결을 정리

## 2. Gateway 세부
### 2.1 주요 컴포넌트
- `GatewayConnection` : TCP 세션, wire codec, 클라이언트 명령 처리
- `LbSession` : gRPC Stream, Gateway ↔ LB 메시지 전달
- `auth::IAuthenticator` : 플러그형 인증 모듈

### 2.2 환경 변수
| 이름 | 설명 | 기본 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | 클라이언트 수신 주소 | `0.0.0.0:6000` |
| `GATEWAY_ID` | Presence/로그 태그 | `gateway-default` |
| `LB_GRPC_ENDPOINT` | LB 주소 | `127.0.0.1:7001` |
| `LB_RETRY_DELAY_MS` | gRPC 재연결 대기 | `3000` |
| `ALLOW_ANONYMOUS`, `AUTH_PROVIDER` | 인증 정책 | `1`, 빈 값 |

### 2.3 운영 팁
- 재연결: gRPC stream 이 끊기면 3초 간격으로 5회 재시도 → 실패 시 클라이언트에 `SESSION_MOVED`
- Pub/Sub self-echo: `GATEWAY_ID` 를 payload 에 포함해 자신이 보낸 메시지를 무시
- 로그: `metric=gateway_lb_reconnect_total` (추후 TODO) 로 재연결 횟수 집계

## 3. Load Balancer 세부
### 3.1 구성 요소
- `LoadBalancerApp` : gRPC 서버, backend 프락시 루프
- `SessionDirectory` : Redis 기반 sticky session
- Instance Registry 클라이언트 : 서버 heartbeat (`gateway/instances/*`) 구독

### 3.2 환경 변수
| 이름 | 설명 | 기본 |
| --- | --- | --- |
| `LB_GRPC_LISTEN` | gRPC 엔드포인트 | `127.0.0.1:7001` |
| `LB_BACKEND_ENDPOINTS` | 정적 backend 목록 | `127.0.0.1:5000` |
| `LB_DYNAMIC_BACKENDS` | Registry 사용 여부 | `0` |
| `LB_REDIS_URI` | Sticky session Redis | 빈 값 |
| `LB_SESSION_TTL` | Sticky TTL(초) | `45` |
| `LB_BACKEND_FAILURE_THRESHOLD` | 실패 허용 횟수 | `3` |
| `LB_BACKEND_COOLDOWN` | 실패 후 재시도 대기 | `5` |
| `LB_BACKEND_IDLE_TIMEOUT` | backend 유휴 종료(초) | `30` |
| `LB_BACKEND_REFRESH_INTERVAL` | registry 재조회 주기 | `5` |

### 3.3 Consistent Hash & Sticky
```text
hash_ring: map<uint32_t, backend_id>
client_key = sha1(client_id)
backend = hash_ring.lower_bound(client_key)
if sticky_dir.has(client) && healthy:
    backend = sticky_dir[client]
```
- sticky 저장: Redis SETNX + TTL → 로컬 캐시 (`CacheEntry{backend, expires}`)
- backend 장애 시 `mark_backend_failure` → 링에서 제거 + sticky 삭제

### 3.4 Idle Close 워크플로우
1. Gateway 와 backend 사이에 30초 이상 데이터가 흐르지 않음
2. Idle watcher thread 가 `last_backend_activity` 를 확인
3. `lb_backend_idle_close_total` 로그(`metric=...`) 남기고 TCP 소켓 close
4. Gateway 는 새로운 backend 로 재연결 (sticky 무시)

## 4. 배포 & 스케일링
| 작업 | 절차 |
| --- | --- |
| Rolling Update | LB → Gateway 순서. 링 TTL 을 2배로 늘리고 readinessProbe 성공 후 다음 단계 진행 |
| Scale-out | HPA 또는 Helm values 로 replica 수 증가. Gateway 는 sticky 정보를 Redis 에서 공유하므로 stateless |
| Multi-region | Redis, RDS 를 region 별로 두고, Gateway/LB 를 각 region 에 배치. 전역 라우팅은 외부 L7(LB or Anycast) 에서 처리 |

## 5. 모니터링
- Gateway 로그: 인증 실패, gRPC 재연결, Pub/Sub lag
- LB 로그: `metric=lb_backend_idle_close_total`, backend success/failure 카운터
- Prometheus 규칙: `sum(increase(lb_backend_idle_close_total[5m]))`, `sum(rate(lb_backend_failure_total[1m]))`
- Grafana 대시보드: "LB Idle Close Rate", "Gateway gRPC Reconnect"

## 6. 장애 시나리오 대응 요약
| 증상 | 경로 | 조치 |
| --- | --- | --- |
| 모든 클라이언트 접속 실패 | Gateway Pods | gRPC endpoint, 인증 실패 로그 확인 |
| 특정 backend 로 트래픽 쏠림 | LB 링 | 링 재구성, sticky cache purge |
| Idle close 급증 | LB watcher | backend CPU, Redis latency, network drop 확인 |

## 7. 참고 문서
- `docs/ops/distributed_routing_draft.md`
- `docs/ops/fallback-and-alerts.md`
- `docs/ops/observability.md`
