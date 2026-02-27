# 코어(Core) 설계 노트

`server_core`는 Knights 공용 런타임으로, 네트워크 I/O·동시성·저장소·운영 유틸리티를 묶은 C++20 라이브러리다. `gateway_app`, `server_app`이 이 모듈을 링크해 동일한 패턴(Hive, ServiceRegistry, metrics 등)을 재사용한다. (HAProxy는 외부 인프라 컴포넌트로, 본 리포의 core를 링크하지 않는다.)

## 1. 설계 목표
- **모듈화**: 채팅 서버뿐 아니라 향후 서비스가 동일한 기반을 공유할 수 있도록 core/net·core/concurrent·core/storage 계층을 명확히 분리한다.
- **확장성**: Hive 기반 I/O, DbWorkerPool, TaskScheduler가 멀티 인스턴스 환경에서 수평 확장을 지원하도록 설계한다.
- **운영 표준화**: CrashHandler, ServiceRegistry, metrics exporter 등 운영 필수 기능을 기본 제공한다.
- **테스트 용이성**: ServiceRegistry와 추상화 레이어 덕분에 gRPC/DB/Redis 컴포넌트를 쉽게 mock 할 수 있다.

## 2. 주요 모듈
### 2.1 네트워크(`core::net`)
- `Hive`/`SessionListener`/`Session` 조합은 `server_app` 중심으로 사용한다.
- `Hive`/`TransportListener`/`TransportConnection` 조합은 `gateway_app`의 클라이언트 수락/브리지 경로에서 사용한다.
- Wire codec과 dispatcher는 opcode 라우팅을 표준화해 신규 메시지를 추가할 때 서비스 코드가 최소화된다.
- Gateway와 Server가 같은 기반을 공유하므로, 연결 처리/백프레셔 정책을 한 곳에서 조정할 수 있다.

#### Dispatcher `processing_place` 실행 정책
- `kInline`: 현재 dispatch 호출 경로에서 핸들러를 즉시 실행한다.
- `kWorker`: `JobQueue`에 작업을 enqueue한 뒤, 세션 직렬 실행 컨텍스트에서 핸들러를 실행한다.
- `kRoomStrand`: 세션 직렬 실행 컨텍스트로 post하여 비동기 실행한다.
- 미지원 `processing_place` 값은 `MSG_ERR(INTERNAL_ERROR)`로 거절한다.
- 운영 관측은 아래 메트릭으로 노출한다.
  - `chat_dispatch_processing_place_calls_total{place=...}`
  - `chat_dispatch_processing_place_reject_total{place=...}`
  - `chat_dispatch_processing_place_exception_total{place=...}`

### 2.2 동시성(`core::concurrent`)
- `TaskScheduler`는 health-check, presence cleanup, metrics flush 등 반복 작업을 예약한다.
- `ThreadManager`, `JobQueue`는 백그라운드 워커와 DbWorkerPool을 안정적으로 관리한다.
- 테스트: `core/tests/task_scheduler_tests.cpp`(추가 예정)에서 scheduling/취소 로직을 검증하도록 구조를 분리해 두었다.

### 2.3 저장소(`core::storage`)
- `DbWorkerPool`은 libpqxx 커넥션을 풀링하고, work 큐를 통해 비동기 DB 작업을 실행한다.
- `redis::client`는 Pub/Sub, Streams, Lua, set-if-equals 등 Redis 기능을 추상화해 Gateway/Server 모두 같은 API를 사용한다.
- DI(ServiceRegistry)와 결합해 mock 백엔드(예: InMemoryStateBackend)를 손쉽게 끼울 수 있다.

### 2.4 상태/유틸(`core::state`, `core::util`)
- `InstanceRegistry`는 Redis 혹은 In-memory backend 위에서 heartbeat와 backend 목록을 관리한다.
- `ServiceRegistry`는 각 모듈이 의존성을 동적으로 주입받을 수 있게 해, 테스트 시 mock을 바인딩하기 쉽다.
- `CrashHandler`, `log` 모듈은 공통 로깅/덤프 정책을 제공하며, `/logs/` 디렉터리에 스택 정보를 남긴다.

### 2.5 설정/관측성
- 과거에는 `.env` 로딩 유틸을 두었으나, 현재 `server_app`/`gateway_app`은 실행 환경에서 주입된 환경 변수를 사용한다.
- `metrics` 서브시스템은 Counter/Gauge/Histogram registry 백엔드를 제공하며, `append_prometheus_metrics()`로 공용 메트릭을 `/metrics`에 합성할 수 있다.
- `append_runtime_core_metrics()`는 서비스별 구현과 무관하게 build info와 함께 공통 런타임 핵심 카운터를 노출하도록 강제한다.
- `AppHost`는 공통 lifecycle phase(`init -> bootstrapping -> running -> stopping -> stopped|failed`)를 관리하고,
  `knights_lifecycle_phase`, `knights_lifecycle_phase_code` 메트릭으로 현재 단계를 노출한다.

## 3. 실행 흐름
1. `.env` 로드 → `ServiceRegistry` 초기화 → DbWorkerPool/Redis/Write-behind/TaskScheduler를 등록한다.
2. `core::net::Session`은 wire decoder로 opcode를 구문 분석한 뒤 Dispatcher에 넘기고, Dispatcher는 ServiceRegistry에서 필요한 핸들러를 찾는다.
3. 백그라운드 작업은 `DbWorkerPool::enqueue`로 큐잉되고, Worker 스레드에서 실행된다.
4. `TaskScheduler`는 health check, presence TTL 정리, metrics 플러시, registry heartbeat 작업을 주기적으로 수행한다.
5. 프로세스 lifecycle은 `AppHost` phase 전이로 표준화되며, readiness/health와 분리되어 운영 상태 추적에 사용된다.

## 4. 향후 확장 항목
| 항목 | 상태 | 메모 |
| --- | --- | --- |
| Hive/Connection 재사용 | Gateway는 `core::net::TransportConnection`, Server는 `core::net::Session` 중심으로 사용 | 필요 시 공통 수명주기 규칙을 추출 가능 |
| 인증 플러그인 | `auth::IAuthenticator` 인터페이스로 구현, 기본은 NoopAuthenticator | 외부 OAuth 연동 시 구현 교체 |
| 스크립팅 훅 | Lua/WASM 플러그인을 로드할 수 있도록 Hook 포인트 정의 | 성능 영향 검토 후 단계 도입 |
| ECS/플러그인 | 채팅 외 모듈을 위한 Entity 시스템은 backlog에 남겨둠 | `docs/roadmap.md` 8번 항목 참조 |
| 관측성 표준화 | `/metrics` + structured log를 기본 제공, OpenTelemetry 추가 검토 | server_app에 우선 적용 후 Gateway/LB로 확장 |

## 5. 테스트 전략
- `core/tests/`에 TaskScheduler, DbWorkerPool 등 핵심 컴포넌트의 단위테스트를 추가해 CI에서 돌린다.
- gRPC/TCP 엔드투엔드 테스트(`tests/integration/`)를 확장해 로그인→채팅→세션 종료까지 자동화한다.
- PowerShell/Bash smoke 스크립트(`scripts/smoke_*.ps1`)로 Redis·Write-behind 경로를 CI에 포함한다.

## 6. 참고 문서
- Sapphire 분석: `docs/sapphire_core_insights.md`
- 전체 아키텍처: `docs/server-architecture.md`
- Gateway & HAProxy 운영: `docs/ops/gateway-and-lb.md`
- Redis/Write-behind 전략: `docs/db/redis-strategy.md`, `docs/db/write-behind.md`
