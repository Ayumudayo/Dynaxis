# Server Architecture Overview

Knights 서버는 `gateway_app` → `load_balancer_app` → `server_app` 순으로 계층화되어 있으며, 세 프로그램 모두 `core/` 라이브러리를 공유한다. Redis/PG/gRPC 조합을 기본으로 하고, 각 계층이 담당하는 역할은 다음과 같다.

```
Client (TCP)
  │
  │ (HELLO/LOGIN/CHAT Frames)
  ▼
gateway_app  ── gRPC Stream ──>  load_balancer_app  ── TCP ──>  server_app
        │                         │                          │
        └──── Redis Presence / Sticky Session / PubSub / Streams ────┘
```

> **Note**: 본 문서는 목표로 하는 MSA 아키텍처를 기술합니다. 현재 개발 단계에서는 `server_app` 단일 프로세스(Monolith)로 동작하며, 추후 `gateway`, `load_balancer` 등으로 분리될 예정입니다.

## 1. 개요
Knights 서버는 고성능 실시간 채팅을 위한 분산 아키텍처를 지향한다.
- **Gateway**: 클라이언트 연결 관리, 프로토콜 변환, 인증 위임.
- **Load Balancer**: 세션 분산, Sticky Routing, 백엔드 헬스 체크.
- **Server (Chat/Logic)**: 실제 비즈니스 로직 처리, 상태 관리.

## 1. 모듈 설명
### server_app
- Boost.Asio 기반 `core::net::Acceptor`/`Session`으로 직접 클라이언트를 관리한다.
- `core::Dispatcher`가 opcode 별 chat handler를 호출하고, `TaskScheduler`는 health check·presence cleanup·registry heartbeat를 실행한다.
- Redis Pub/Sub 및 Streams(write-behind)를 사용해 브로드캐스트와 이벤트 적재를 처리한다.

### gateway_app
- 클라이언트 TCP 연결을 terminates하고, Load Balancer의 gRPC Stream과 브리징한다.
- HELLO/heartbeat/로그인/채팅 payload를 그대로 전달하며, self-echo 필터와 rate limit를 수행한 뒤 LB에 위임한다.

### load_balancer_app
- gRPC Stream으로 Gateway를 수신하고, Consistent Hash + Sticky Session 전략으로 backend(server_app)를 선택한다.
- Redis Session Directory(`gateway/session:{client}`)와 Instance Registry(`gateway/instances/*`)를 사용해 동적 backend 목록을 관리한다.

## 2. server_app 부팅 순서
1. `.env` 로드(`server/core/config/dotenv.hpp`).
2. `asio::io_context`, `TaskScheduler`, `DbWorkerPool`, `BufferManager` 초기화.
3. 환경 변수(`DB_URI`, `REDIS_URI`, `WRITE_BEHIND_ENABLED`, `USE_REDIS_PUBSUB` 등) 파싱.
4. DB/Redis 커넥션 풀, write-behind emitter, Redis Pub/Sub 구독자 생성.
5. Redis Instance Registry에 backend heartbeat 등록 (`SERVER_REGISTRY_PREFIX`, `SERVER_ADVERTISE_HOST/PORT`).
6. `ChatService::init`으로 라우터/저장소 의존성 주입.
7. `core::Acceptor` 시작, health-check/presence clean-up/metrics exporter 스케줄링.
8. SIGINT/SIGTERM 수신 시 graceful shutdown.

## 3. 메시지 플로우
1. Client ↔ Gateway: TCP Frame(HELLO, LOGIN, CHAT) 교환.
2. Gateway ↔ Load Balancer: gRPC Stream으로 payload 전달/회수.
3. Load Balancer ↔ Server: TCP 프록시 연결을 생성해 실제 채팅 세션을 유지.
4. 서버에서 opcode 핸들러가 DB/Redis를 갱신하고, 결과를 Gateway를 통해 다시 전달.
5. `USE_REDIS_PUBSUB=1`이면 Redis로 브로드캐스트하고 다른 서버가 구독한다.
6. `WRITE_BEHIND_ENABLED=1`이면 Redis Streams에 이벤트를 적재하고 `wb_worker`가 Postgres에 반영한다.

## 4. 구성 요소 간 책임
- **Gateway**: `GATEWAY_ID`, `REDIS_CHANNEL_PREFIX`, `LB_GRPC_ENDPOINT` 환경 변수를 사용해 self-echo와 LB 연결을 제어한다.
- **Load Balancer**: `LB_DYNAMIC_BACKENDS`, `LB_BACKEND_REFRESH_INTERVAL`, `LB_BACKEND_REGISTRY_PREFIX`로 backend 목록을 갱신한다. Sticky session TTL(`LB_SESSION_TTL`)과 실패/쿨다운 정책(`LB_BACKEND_FAILURE_THRESHOLD`/`LB_BACKEND_COOLDOWN`)을 관리한다.
- **Server**: `SERVER_ADVERTISE_HOST/PORT`, `SERVER_REGISTRY_PREFIX`, `SERVER_REGISTRY_TTL`, `SERVER_HEARTBEAT_INTERVAL`로 Instance Registry에 자신을 등록하고 갱신한다.
- **Persistence**: PostgreSQL은 SoR, Redis는 캐시/팬아웃 레이어. 자세한 전략은 `docs/db/redis-strategy.md` 참고.
- **Persistence**: PostgreSQL은 SoR, Redis는 캐시/팬아웃 레이어. 자세한 전략은 `docs/db/redis-strategy.md` 참고.

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
| server_app | `SERVER_BIND_ADDR`, `SERVER_PORT` | TCP 리스너 |
|  | `DB_URI`, `DB_POOL_MIN/MAX` | Postgres 커넥션 |
|  | `REDIS_URI`, `REDIS_POOL_MAX` | Redis 커넥션 |
|  | `WRITE_BEHIND_ENABLED`, `WB_*` | Write-behind 배치/딜레이/DLQ |
|  | `USE_REDIS_PUBSUB`, `GATEWAY_ID`, `REDIS_CHANNEL_PREFIX` | 브로드캐스트 제어 |
|  | `PRESENCE_TTL_SEC` | Presence TTL |
|  | `METRICS_PORT` | `/metrics` 포트 |
|  | `SERVER_ADVERTISE_HOST/PORT`, `SERVER_INSTANCE_ID` | Instance Registry 등록 정보 |
|  | `SERVER_REGISTRY_PREFIX/TTL`, `SERVER_HEARTBEAT_INTERVAL` | Registry heartbeat 옵션 |
| gateway_app | `GATEWAY_LISTEN` | TCP 리스너 |
|  | `LB_GRPC_ENDPOINT` | Load Balancer gRPC 주소 |
|  | `LB_GRPC_REQUIRED`, `LB_RETRY_DELAY_MS` | 재연결 정책 |
| load_balancer_app | `LB_GRPC_LISTEN` | gRPC 리스너 |
|  | `LB_BACKEND_ENDPOINTS` | 정적 backend 목록 |
|  | `LB_REDIS_URI`, `LB_SESSION_TTL` | Redis Session Directory |
|  | `LB_BACKEND_REFRESH_INTERVAL` | Registry poll 주기 |
|  | `LB_DYNAMIC_BACKENDS`, `LB_BACKEND_REGISTRY_PREFIX` | 동적 backend 설정 |
|  | `LB_BACKEND_FAILURE_THRESHOLD`, `LB_BACKEND_COOLDOWN`, `LB_HEARTBEAT_INTERVAL` | 실패/쿨다운/heartbeat |

## 6. 운영 체크리스트
- Gateway/LB 로그에서 `applied backend snapshot`과 heartbeat 성공 메시지를 확인한다.
- Redis Pub/Sub과 Streams pending 길이를 `/metrics` 또는 `redis-cli xinfo stream`으로 모니터링한다.
- 서버 종료 시 Instance Registry 엔트리를 삭제하고, 재시작 후 새 인스턴스 ID를 배정한다.
- `scripts/run_all.ps1`로 Gateway+LB+Server+Client를 동시에 구동해 E2E 연동을 주기적으로 확인한다.

## 7. 참고 자료
- Protocol/프레임: `docs/protocol.md`
- Redis/Write-behind: `docs/db/redis-strategy.md`, `docs/db/write-behind.md`
- 운영 가이드: `docs/ops/gateway-and-lb.md`, `docs/ops/runbook.md`, `docs/ops/observability.md`
