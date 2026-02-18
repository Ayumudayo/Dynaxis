# Server Architecture Overview

Knights의 현재 실행 토폴로지는 `gateway_app`와 `server_app` 중심이며, **외부 TCP 로드밸런서(예: HAProxy)** 를 두어 `gateway_app` 인스턴스를 수평 확장하는 배포 모델을 전제로 한다.

```
Client (TCP)
  │
  │ (HELLO/LOGIN/CHAT Frames)
  ▼
HAProxy (TCP, L4)  ──>  gateway_app  ── TCP ──>  server_app
                      │                 │
                      └── Redis (Instance Registry / SessionDirectory / PubSub / Streams)
```

> **Note**: 과거 문서에는 `load_balancer_app`(gRPC Stream 기반 커스텀 LB) 계층이 포함되어 있었으나, 현재 코드/빌드 타깃에는 존재하지 않는다. 본 문서는 **HAProxy 기반 배포**와 **Gateway 내부 라우팅(Sticky + Least Connections)** 을 기준으로 정리한다.

## 1. 개요
Knights는 고성능 실시간 채팅을 위한 분산 스택을 지향한다.
- **Edge Load Balancer (HAProxy)**: 클라이언트 TCP 연결을 여러 `gateway_app` 인스턴스로 분산한다.
- **Gateway (`gateway_app`)**: 클라이언트 TCP 연결을 terminate하고 인증/세션 관리를 수행한다. Redis Instance Registry를 조회해 backend(`server_app`)를 선택하고 1:1 TCP 브리지를 구성한다.
- **Server (`server_app`)**: 채팅 비즈니스 로직 처리, Redis Pub/Sub(팬아웃), Redis Streams(write-behind) 이벤트 발행, PostgreSQL 적재 파이프라인을 담당한다.

### 1.1 왜 L4 LB와 Gateway를 분리하는가
- HAProxy 같은 L4 LB는 TCP 분산에 최적화되어 있지만, 로그인 프레임 해석/인증/세션 stickiness 같은 애플리케이션 정책은 다루지 않는다.
- 반대로 Gateway가 L4 역할까지 모두 대체하면 외부 트래픽 분산/헬스체크/운영 표준 도구와의 호환성이 떨어진다.
- 따라서 "네트워크 분산(L4)"과 "애플리케이션 연결 제어(L7 성격)"를 분리해, 확장성과 운영 단순성을 동시에 확보한다.

## 2. 모듈 설명
### server_app
- Boost.Asio 기반 `core::net::SessionListener`/`core::net::Session`(legacy: `core::Acceptor`/`core::Session`)으로 클라이언트(=Gateway의 `BackendConnection`) 연결을 처리한다.
- `core::Dispatcher`가 opcode 별 chat handler를 호출하고, `TaskScheduler`가 health check·presence clean-up·registry heartbeat를 실행한다.
- Redis Pub/Sub 및 Streams(write-behind)를 사용해 브로드캐스트와 이벤트 적재를 처리한다.

### gateway_app
- (보통 HAProxy 뒤에서) 클라이언트 TCP 연결을 terminate한다.
- `core::net::TransportListener`가 클라이언트 연결을 수락하고, 각 연결은 `GatewayConnection`(`core::net::TransportConnection` 파생)으로 처리한다.
- Redis Instance Registry를 조회해 backend(server_app)를 선택하고, `BackendConnection`으로 TCP 연결을 생성한다.
- `SessionDirectory`(Redis 기반)를 사용해 클라이언트 재접속 시 동일 backend로 라우팅(Sticky)한다.

## 3. server_app 부팅 순서
1. 커맨드라인/환경 변수에서 설정을 로드한다.
2. `asio::io_context`, `TaskScheduler`, `DbWorkerPool`, `BufferManager` 초기화.
3. DB/Redis 커넥션 및 옵션 구성.
4. Redis Instance Registry에 서버 인스턴스를 등록하고, 주기적으로 heartbeat(upsert)를 수행한다.
5. `ChatService`/라우터를 초기화한 뒤 `core::net::SessionListener`를 시작한다.
6. SIGINT/SIGTERM 수신 시 graceful shutdown(레지스트리 엔트리 제거, Pub/Sub 중지 등).

## 4. 메시지 플로우
1. Client ↔ HAProxy ↔ Gateway: TCP Frame(HELLO, LOGIN, CHAT) 교환.
2. Gateway ↔ Server: Gateway가 선택한 backend(server_app)로 TCP 연결을 생성하고 payload를 중계한다.
3. `USE_REDIS_PUBSUB=1`이면 서버는 Redis로 브로드캐스트하고 다른 서버 인스턴스가 구독한다.
4. Write-behind 경로가 활성화되어 있으면 서버는 Redis Streams에 이벤트를 적재하고 `wb_worker`가 Postgres에 반영한다.

### 4.1 왜 Redis Pub/Sub 팬아웃이 필요한가
- 다중 `server_app` 환경에서는 같은 방 사용자들이 서로 다른 인스턴스에 붙어 있을 수 있다.
- 한 인스턴스에서만 로컬 브로드캐스트하면 다른 인스턴스 사용자에게 메시지가 전달되지 않는다.
- Redis Pub/Sub을 공통 백플레인으로 두면 서버 간 메시지 동기화 경로를 단순하게 유지할 수 있다.

## 5. 구성 요소 간 책임
- **HAProxy**: 외부 TCP 로드밸런서. 여러 `gateway_app` 인스턴스로 연결을 분산하며, 애플리케이션 프로토콜(opcode)은 해석하지 않는다.
- **Gateway**: `GATEWAY_LISTEN`, `GATEWAY_ID`, `REDIS_URI`로 리스너/식별자/Redis 연결을 구성한다. Redis Instance Registry + `active_sessions` 기반으로 backend를 선택(Least Connections)하고, `SessionDirectory`로 sticky routing을 수행한다.
- **Server**: `PORT`, `SERVER_ADVERTISE_HOST/PORT`, `SERVER_REGISTRY_PREFIX/TTL`, `SERVER_HEARTBEAT_INTERVAL`로 Instance Registry에 자신을 등록/갱신한다.
- **Persistence**: PostgreSQL은 SoR, Redis는 캐시/팬아웃/스트림 레이어. 자세한 전략은 `docs/db/redis-strategy.md` 참고.

## 5. Privacy & Audit (Room Rotation)
Knights는 채팅 내역의 영구 보존(Audit)과 프라이버시(Privacy)를 동시에 만족하기 위해 **Room Rotation** 전략을 사용한다.
- **방 닫기 (Soft Delete)**: 방의 마지막 유저가 나가면 해당 방은 `is_active=false`로 설정되고 `closed_at`이 기록된다.
- **새로운 UUID 발급**: 이후 동일한 이름의 방이 생성되면, 이전 방(Old UUID)은 무시되고 새로운 UUID를 가진 방이 생성된다.
- **결과**: 이전 채팅 내역은 DB에 보존되지만, 새로운 방에서는 조회되지 않아 프라이버시가 보호된다.

## 6. Guest Identity
게스트 유저("guest-123")의 경우, 클라이언트가 자신의 식별자를 알 수 있도록 `StateSnapshot` 프로토콜에 `your_name` 필드를 추가했다.
- 서버는 스냅샷 전송 시 세션에 할당된 이름을 `your_name`에 담아 보낸다.
- 클라이언트는 이를 받아 자신의 닉네임으로 설정하고, 메시지 전송 시 "me" 식별에 사용한다.

## 7. 환경 변수 요약
| 애플리케이션 | 주요 변수 | 설명 |
| --- | --- | --- |
| server_app | `PORT` | TCP 리스너(또는 실행 인자) |
|  | `DB_URI`, `DB_POOL_MIN/MAX` | Postgres 커넥션 |
|  | `REDIS_URI`, `REDIS_POOL_MAX` | Redis 커넥션 |
|  | `USE_REDIS_PUBSUB`, `GATEWAY_ID`, `REDIS_CHANNEL_PREFIX` | 브로드캐스트/self-echo 제어 |
|  | `METRICS_PORT` | `/metrics` 포트 |
|  | `SERVER_ADVERTISE_HOST/PORT`, `SERVER_INSTANCE_ID` | Instance Registry 등록 정보 |
|  | `SERVER_REGISTRY_PREFIX/TTL`, `SERVER_HEARTBEAT_INTERVAL` | Registry heartbeat 옵션 |
| gateway_app | `GATEWAY_LISTEN` | TCP 리스너 |
|  | `GATEWAY_ID` | 식별자(로그/Presence 등) |
|  | `REDIS_URI` | Redis 연결 |
|  | `METRICS_PORT` | `/metrics` 포트 |

## 8. 운영 체크리스트
- HAProxy 헬스체크 실패율/백엔드 다운을 먼저 확인한다.
- Gateway 로그에서 backend 선택/연결 실패가 반복되는지 확인한다.
- Redis Pub/Sub과 Streams pending 길이를 `/metrics` 또는 `redis-cli xinfo stream`으로 모니터링한다.
- 서버 종료 시 Instance Registry 엔트리를 삭제하고, 재시작 후 새 인스턴스 ID를 배정한다.

## 9. 참고 자료
- Protocol/프레임: `docs/protocol.md`
- Redis/Write-behind: `docs/db/redis-strategy.md`, `docs/db/write-behind.md`
- 운영 가이드: `docs/ops/gateway-and-lb.md`, `docs/ops/runbook.md`, `docs/ops/observability.md`
