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

## 5. 환경 변수 요약
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
