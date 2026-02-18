# Gateway & HAProxy 운영 가이드 (상세)

Gateway를 수평 확장하기 위해 외부 TCP(L4) 로드밸런서(예: HAProxy)를 사용하는 배포/운영 가이드다.

> Note: 과거 문서에는 `load_balancer_app`(gRPC Stream 기반 커스텀 LB)가 포함되어 있었으나, 현재 코드/빌드 타깃에는 존재하지 않는다.

## 1. 전체 흐름
```
Client --TCP--> HAProxy --TCP--> Gateway --TCP--> server_app
                          │
                          ├─ Redis (SessionDirectory, Instance Registry)
                          └─ Prometheus (로그 지표)
```
1. HAProxy는 클라이언트 TCP 연결을 여러 Gateway 인스턴스로 분산한다.
2. Gateway는 인증 후 Redis Instance Registry를 조회해 backend(server_app)를 선택한다.
3. Gateway는 선택된 backend로 TCP 연결(`BackendConnection`)을 생성하고 payload를 중계한다.
4. Sticky routing은 Redis SessionDirectory(`gateway/session/<client_id>`)를 통해 유지된다.

## 2. Gateway 세부
### 2.1 주요 컴포넌트
- `GatewayConnection` : TCP 세션, wire codec, 클라이언트 명령 처리
- `BackendConnection` : Gateway ↔ server_app TCP 중계 세션
- `auth::IAuthenticator` : 플러그형 인증 모듈

### 2.2 환경 변수
| 이름 | 설명 | 기본 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | 클라이언트 수신 주소 | `0.0.0.0:6000` |
| `GATEWAY_ID` | Presence/로그 태그 | `gateway-default` |
| `REDIS_URI` | Instance Registry/SessionDirectory Redis | `tcp://127.0.0.1:6379` |
| `METRICS_PORT` | `/metrics` 포트 | `6001` |
| `GATEWAY_BACKEND_CONNECT_TIMEOUT_MS` | backend connect timeout(ms) | `5000` |
| `GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES` | backend 전송 대기 큐 상한 바이트 | `262144` |
| `ALLOW_ANONYMOUS`, `AUTH_PROVIDER` | 인증 정책 | `1`, 빈 값 |

### 2.3 운영 팁
- Pub/Sub self-echo: `GATEWAY_ID` 를 payload 에 포함해 자신이 보낸 메시지를 무시
- Gateway가 backend를 선택할 때는 Redis Instance Registry의 `active_sessions`(least-connections)와 SessionDirectory sticky를 함께 사용한다.
- Instance Registry prefix는 server_app과 gateway_app이 동일해야 한다.

## 3. HAProxy 세부
HAProxy는 TCP 레벨에서만 Gateway로 분산한다. (애플리케이션 opcode는 Gateway/Server가 처리)

로컬 검증은 `docker/stack/docker-compose.yml` 을 권장한다.
HAProxy 설정은 `docker/stack/haproxy/haproxy.cfg` 로 관리한다.

### 3.1 로컬 개발용 예시 설정
아래 예시는 HAProxy 컨테이너가 `gateway-1`, `gateway-2` 컨테이너로 TCP를 분산하는 형태다.

```haproxy
global
  maxconn 10000

defaults
  mode tcp
  timeout connect 3s
  timeout client  60s
  timeout server  60s

frontend fe_gateway
  bind 0.0.0.0:6000
  default_backend be_gateway

backend be_gateway
  balance roundrobin
  server gw1 gateway-1:6000 check
  server gw2 gateway-2:6000 check

listen stats
  bind 0.0.0.0:8404
  mode http
  stats enable
  stats uri /
```

### 3.2 운영 팁
- Gateway는 stateful TCP를 terminate하므로, HAProxy는 “연결 단위”로 분산한다.
- Gateway의 sticky routing은 Redis(SessionDirectory)로 구현되어 있으므로, HAProxy가 어떤 Gateway로 보내더라도 동일 client_id에 대해 동일 backend로 라우팅될 수 있다.

## 4. 배포 & 스케일링
| 작업 | 절차 |
| --- | --- |
| Rolling Update | HAProxy 설정 반영 → Gateway 순서. readinessProbe 성공 후 다음 단계 진행 |
| Scale-out | Gateway replica 수 증가. sticky 정보는 Redis에 저장되므로 Gateway는 무상태(stateless)로 확장 가능 |
| Multi-region | Redis, RDS 를 region 별로 두고, Gateway/Edge LB(예: HAProxy)를 각 region 에 배치. 전역 라우팅은 외부 L7(LB or Anycast) 에서 처리 |

## 5. 모니터링
- Gateway 로그: 인증 실패, backend 선택/연결 실패, Pub/Sub lag, send queue overflow
- Gateway metrics: `gateway_backend_resolve_fail_total`, `gateway_backend_connect_fail_total`, `gateway_backend_connect_timeout_total`, `gateway_backend_write_error_total`, `gateway_backend_send_queue_overflow_total`
- HAProxy 로그/통계: 프런트/백엔드 에러율, 다운된 Gateway 백엔드 수

## 6. 장애 시나리오 대응 요약
| 증상 | 경로 | 조치 |
| --- | --- | --- |
| 모든 클라이언트 접속 실패 | HAProxy/Gateway | HAProxy 백엔드 다운 여부 + Gateway 인증 실패 로그 확인 |
| 특정 backend 로 트래픽 쏠림 | Gateway 라우팅 | Instance Registry active_sessions, SessionDirectory sticky 점검 |

## 7. 참고 문서
- `docs/ops/distributed_routing_draft.md`
- `docs/ops/fallback-and-alerts.md`
- `docs/ops/observability.md`
