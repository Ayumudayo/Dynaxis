# 게이트웨이와 HAProxy 운영 안내서

이 문서는 Gateway를 수평 확장하기 위해 외부 TCP(L4) 로드밸런서, 예를 들어 HAProxy를 사용하는 배포와 운영 방식을 설명한다. 핵심은 단순히 "앞에 LB를 둔다"가 아니다. 어떤 계층이 연결을 끝내고, 어떤 계층이 backend를 선택하며, 어느 상태를 Redis에 두는지가 분명해야 운영 중 장애 원인을 빠르게 좁힐 수 있다.

> 참고: 과거 문서에는 `load_balancer_app`(gRPC Stream 기반 커스텀 LB)가 포함되어 있었으나, 현재 코드와 빌드 타깃에는 존재하지 않는다.

## 1. 전체 흐름

```text
Client --TCP--> HAProxy --TCP--> Gateway --TCP--> server_app
                          │
                          ├─ Redis (SessionDirectory, Instance Registry)
                          └─ Prometheus (로그 지표)
```

1. HAProxy는 클라이언트 TCP 연결을 여러 Gateway 인스턴스로 분산한다.
2. Gateway는 인증 뒤 Redis Instance Registry를 조회해 backend(`server_app`)를 선택한다.
3. Gateway는 선택된 backend로 TCP 연결(`BackendConnection`)을 생성하고 payload를 중계한다.
4. 고정 라우팅(sticky routing)은 Redis SessionDirectory(`gateway/session/<client_id>`)를 통해 유지된다.

server 역할 요약:

- `server_app`은 chat/runtime state, Redis Pub/Sub fanout, Redis Streams write-behind 발행을 소유한다.
- gateway와 L4 계층은 연결과 라우팅을 담당하고, room/runtime state는 server가 소유한다.

이 구분이 중요한 이유는 room 상태와 사용자 의미론까지 gateway 쪽으로 끌어오면 edge 계층이 비대해지기 때문이다. 반대로 server가 연결과 분산까지 떠안으면 수평 확장과 장애 격리가 어려워진다.

> 구현 경계: shared instance-discovery 계약(`InstanceRecord`, selector, backend interface)과 world lifecycle policy 계약은 `server_core`의 stable `core::discovery` 표면에 있고, Redis/Consul adapter와 sticky `SessionDirectory` 구현은 여전히 internal, app-owned 경계에 남아 있다.

## 2. Gateway 세부

### 2.1 주요 컴포넌트

- `GatewayConnection`: TCP 세션, wire codec, 클라이언트 명령 처리
- `BackendConnection`: Gateway와 `server_app` 사이의 TCP 중계 세션
- `auth::IAuthenticator`: 플러그형 인증 모듈

이 구성요소를 나누는 이유는 "클라이언트와 붙는 연결"과 "backend로 붙는 연결"이 실패했을 때 대응이 다르기 때문이다. 하나의 타입이 두 역할을 모두 맡기 시작하면 timeout, queue overflow, retry 정책이 뒤섞인다.

### 2.2 환경 변수

| 이름 | 설명 | 기본 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | 클라이언트 수신 주소 | `0.0.0.0:6000` |
| `GATEWAY_ID` | Presence/로그 태그 | `gateway-default` |
| `REDIS_URI` | Instance Registry/SessionDirectory Redis | `tcp://127.0.0.1:6379` |
| `METRICS_PORT` | `/metrics` 포트 | `6001` |
| `GATEWAY_BACKEND_CONNECT_TIMEOUT_MS` | backend connect timeout(ms) | `5000` |
| `GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES` | backend 전송 대기 큐 상한 바이트 | `262144` |
| `GATEWAY_BACKEND_CIRCUIT_BREAKER_ENABLED` | backend circuit breaker 활성화(1/0) | `1` |
| `GATEWAY_BACKEND_CIRCUIT_FAIL_THRESHOLD` | circuit open 연속 실패 임계치 | `5` |
| `GATEWAY_BACKEND_CIRCUIT_OPEN_MS` | circuit open 유지 시간(ms) | `10000` |
| `GATEWAY_BACKEND_CONNECT_RETRY_BUDGET_PER_MIN` | backend connect 재시도 예산(분당) | `120` |
| `GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MS` | backend connect 재시도 백오프 시작(ms) | `200` |
| `GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MAX_MS` | backend connect 재시도 백오프 상한(ms) | `2000` |
| `GATEWAY_INGRESS_TOKENS_PER_SEC` | ingress token bucket 초당 토큰 | `200` |
| `GATEWAY_INGRESS_BURST_TOKENS` | ingress token bucket burst | `400` |
| `GATEWAY_INGRESS_MAX_ACTIVE_SESSIONS` | 동시 backend 세션 상한 | `50000` |
| `ALLOW_ANONYMOUS`, `AUTH_PROVIDER` | 인증 정책 | `1`, 빈 값 |

이 표는 단순 옵션 목록이 아니라 보호 장치 목록에 가깝다. connect timeout, send queue 상한, circuit breaker, retry budget이 왜 필요한지를 이해해야 gateway 장애를 "네트워크 문제"와 "과부하 제어 문제"로 나눠 볼 수 있다.

### 2.3 운영 팁

- Pub/Sub self-echo: `GATEWAY_ID`를 payload에 포함해 자신이 보낸 메시지를 무시한다.
- Gateway가 backend를 선택할 때는 Redis Instance Registry의 `active_sessions`(least-connections)와 SessionDirectory sticky를 함께 사용한다.
- Instance Registry prefix는 `server_app`과 `gateway_app`이 동일해야 한다.

이 세 조건이 흐트러지면 메시지가 자기 자신에게 다시 돌아오거나, sticky 라우팅이 깨지거나, registry 자체를 못 읽는 문제가 생긴다. 운영에서 보기엔 비슷한 "연결 이상"처럼 보여도 원인은 완전히 다르다.

## 3. HAProxy 세부

HAProxy는 TCP 레벨에서만 Gateway로 분산한다. 애플리케이션 opcode는 Gateway와 Server가 처리한다.

로컬 검증은 `docker/stack/docker-compose.yml`을 권장한다.
HAProxy 설정은 compose 환경 변수 `HAPROXY_CONFIG`로 선택한다.

- 개발 기본값: `HAPROXY_CONFIG=haproxy.cfg`
- 운영 템플릿: `HAPROXY_CONFIG=haproxy.tls13.cfg`

HAProxy를 TCP 레벨에만 두는 이유는 애플리케이션 의미를 LB까지 올리지 않기 위해서다. L4 계층은 연결 분산과 health check에 집중하고, 세션 의미와 backend 선택은 Gateway가 소유하는 편이 경계가 선명하다.

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

### 3.2 TLS 1.3/mTLS 운영 정책

- 기본 edge listener는 TLS 1.3을 강제한다. (`ssl-min-ver TLSv1.3`)
- 레거시 클라이언트 예외는 별도 포트 listener(TLS 1.2 only)로 분리하고, SNI allowlist로 대상 도메인을 제한한다.
- LB -> gateway/backend 내부 hop은 `ssl verify required`와 내부 CA로 mTLS를 강제한다.
- 인증서 갱신은 30일, 14일, 7일 윈도우로 운영한다.
  - 30일: 일정 확정
  - 14일: 리허설
  - 7일: 즉시 교체

검증/운영 템플릿:

- `docker/stack/haproxy/haproxy.tls13.cfg`

정책을 이렇게 나누는 이유는 클라이언트 호환성과 내부 신뢰 경계를 같은 규칙으로 다루면 안 되기 때문이다. 외부 edge는 클라이언트 다양성을 고려해야 하지만, 내부 hop은 더 강한 검증을 걸어도 된다.

### 3.3 운영 팁

- Gateway는 상태를 가진 TCP 연결을 종료하므로, HAProxy는 "연결 단위"로 분산한다.
- Gateway의 고정 라우팅(sticky routing)은 Redis(SessionDirectory)로 구현되어 있으므로, HAProxy가 어떤 Gateway로 보내더라도 동일 `client_id`에 대해 동일 backend로 라우팅될 수 있다.

이 구조가 좋은 이유는 LB가 세션 의미를 몰라도 된다는 점이다. 세션 의미를 Redis와 Gateway에 모으면 LB는 단순하고 안정적으로 유지할 수 있다.

## 4. 배포와 확장

| 작업 | 절차 |
| --- | --- |
| 롤링 업데이트 | HAProxy 설정 반영 -> Gateway 순서. readinessProbe 성공 뒤 다음 단계 진행 |
| 수평 확장 | Gateway replica 수 증가. sticky 정보는 Redis에 저장되므로 Gateway는 무상태(stateless)에 가깝게 확장 가능 |
| 멀티 리전 | Redis, RDS를 리전별로 두고, Gateway와 Edge LB(예: HAProxy)를 각 리전에 배치. 전역 라우팅은 외부 L7(LB 또는 Anycast)에서 처리 |

업데이트 순서를 HAProxy 먼저, Gateway 나중으로 두는 이유는 edge가 새 gateway를 받아들일 준비가 된 뒤에 실제 세션 수용 범위를 넓혀야 하기 때문이다. 반대로 순서를 바꾸면 health check와 실제 연결 흐름이 잠시 어긋날 수 있다.

## 5. 모니터링

- Gateway 로그: 인증 실패, backend 선택/연결 실패, Pub/Sub lag, send queue overflow
- Gateway 메트릭: `gateway_backend_resolve_fail_total`, `gateway_backend_connect_fail_total`, `gateway_backend_connect_timeout_total`, `gateway_backend_write_error_total`, `gateway_backend_send_queue_overflow_total`
- 회복탄력성과 과부하 제어 지표:
  - `gateway_backend_circuit_open_total`, `gateway_backend_circuit_reject_total`, `gateway_backend_circuit_open`
  - `gateway_backend_connect_retry_total`, `gateway_backend_retry_budget_exhausted_total`
  - `gateway_ingress_reject_not_ready_total`, `gateway_ingress_reject_rate_limit_total`, `gateway_ingress_reject_session_limit_total`, `gateway_ingress_reject_circuit_open_total`
- HAProxy 로그와 통계: 프런트/백엔드 에러율, 다운된 Gateway 백엔드 수

이 계층을 함께 봐야 하는 이유는 gateway 문제처럼 보이는 장애가 사실 HAProxy health check 문제이거나, 반대로 LB 문제처럼 보이는 증상이 gateway retry budget 소진일 수 있기 때문이다.

## 6. 장애 시나리오 대응 요약

| 증상 | 경로 | 조치 |
| --- | --- | --- |
| 모든 클라이언트 접속 실패 | HAProxy/Gateway | HAProxy 백엔드 다운 여부와 Gateway 인증 실패 로그 확인 |
| 특정 backend로 트래픽 쏠림 | Gateway 라우팅 | Instance Registry `active_sessions`, SessionDirectory sticky 점검 |

장애 표를 별도로 두는 이유는 실전에서는 아키텍처 설명보다 "이 증상일 때 어디부터 볼 것인가"가 더 중요하기 때문이다. 운영자가 시작 지점을 바로 잡으면 복구 시간도 짧아진다.

## 7. 참고 문서

- `docs/ops/fallback-and-alerts.md`
- `docs/ops/observability.md`
