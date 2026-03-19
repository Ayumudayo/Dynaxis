# 코어(Core) 설계 노트

`server_core`는 Dynaxis 공용 런타임으로, 네트워크 I/O·동시성·저장소·운영 유틸리티를 묶은 C++20 라이브러리다. `gateway_app`, `server_app`이 이 모듈을 링크해 동일한 패턴(Hive, ServiceRegistry, metrics 등)을 재사용한다. (HAProxy는 외부 인프라 컴포넌트로, 본 리포의 core를 링크하지 않는다.)

## 1. 설계 목표
- **모듈화**: 채팅 서버뿐 아니라 향후 서비스가 동일한 기반을 공유할 수 있도록 core/net·core/concurrent·core/storage 계층을 명확히 분리한다.
- **확장성**: Hive 기반 I/O, DbWorkerPool, TaskScheduler가 멀티 인스턴스 환경에서 수평 확장을 지원하도록 설계한다.
- **운영 표준화**: CrashHandler, ServiceRegistry, metrics exporter 등 운영 필수 기능을 기본 제공한다.
- **테스트 용이성**: ServiceRegistry와 추상화 레이어 덕분에 gRPC/DB/Redis 컴포넌트를 쉽게 mock 할 수 있다.

## 1.1 식별자(Identity) 계약

- 시스템 식별자는 UUID를 기준으로 한다:
  - `user_id`
  - `room_id`
  - `session_id`
- 이름(`user.name`, `room.name`)은 라벨이며, 프로토콜/키/저장소의 canonical 식별자가 아니다.
- 프로토콜, Redis 키, 저장소 조인 경계에서는 이름 의존 대신 UUID를 우선 사용한다.
- 룸/사용자/세션의 검색 편의는 이름 인덱스로 해결하고, 정합성은 UUID에 의존한다.

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
- `core::storage`는 generic transaction 경계(`IUnitOfWork`), connection factory(`IConnectionPool`), shared Redis client contract(`core::storage::redis::IRedisClient`), `DbWorkerPool` 같은 비동기 실행 seam을 소유한다.
- 채팅 도메인 repository DTO/인터페이스와 Postgres SQL 구현은 `server/storage/*`에 두어 core가 채팅 스키마에 직접 결합되지 않게 유지한다.
- concrete Redis factory/redis-plus-plus adapter는 `server/storage/*`에 두고, Gateway/Server/worker는 shared client contract만 직접 본다.
- DI(ServiceRegistry)와 결합해 mock 백엔드(예: InMemoryStateBackend)를 손쉽게 끼울 수 있다.

### 2.4 상태/유틸(`core::state`, `core::util`)
- `core::state`는 `InstanceRecord`, selector helper, `IInstanceStateBackend`, `InMemoryStateBackend` 같은 shared discovery contract를 제공한다.
- Redis 기반 instance registry adapter/factory의 canonical seam은 `core/state`로 이동했지만, 현재는 `Transitional/Internal` ownership으로 관리한다.
- Consul 기반 instance registry adapter는 `server/state/*`에 남고, sticky `SessionDirectory` 구현은 `gateway/*`에 남는다.
- `ServiceRegistry`는 각 모듈이 의존성을 동적으로 주입받을 수 있게 해, 테스트 시 mock을 바인딩하기 쉽다.
- `CrashHandler`, `log` 모듈은 공통 로깅/덤프 정책을 제공하며, `/logs/` 디렉터리에 스택 정보를 남긴다.

### 2.5 확장성(`core::plugin`, `core::scripting`)
- `core::plugin`과 `core::scripting`은 plugin/Lua extensibility를 위한 platform mechanism 계층이다.
- `server/`는 chat hook ABI, plugin chain policy, Lua host bindings 같은 service-specific contract만 소유한다.
- reusable mechanism layer는 이제 `Stable`로 관리하고, chat-specific ABI/binding은 `Transitional`로 분리해 관리한다.

### 2.6 설정/관측성
- 과거에는 `.env` 로딩 유틸을 두었으나, 현재 `server_app`/`gateway_app`은 실행 환경에서 주입된 환경 변수를 사용한다.
- `metrics` 서브시스템은 Counter/Gauge/Histogram registry 백엔드를 제공하며, `append_prometheus_metrics()`로 공용 메트릭을 `/metrics`에 합성할 수 있다.
- `append_runtime_core_metrics()`는 서비스별 구현과 무관하게 build info와 함께 공통 런타임 핵심 카운터를 노출하도록 강제한다.
- `AppHost`는 공통 lifecycle phase(`init -> bootstrapping -> running -> stopping -> stopped|failed`)를 관리하고,
  `runtime_lifecycle_phase`, `runtime_lifecycle_phase_code` 메트릭으로 현재 단계를 노출한다.

### 2.7 조합 헬퍼 타깃(Composition Helper Targets)
- `server_app_backends`, `gateway_backends`, `admin_app_backends`, `wb_common_redis_factory`는 reusable engine module이 아니라 각 실행 파일의 composition helper로 취급한다.
- 이 타깃들은 프로세스별 설정/수명주기/운영 맥락을 `server_storage_pg_factory`, `server_storage_redis_factory`, 그리고 core-owned Redis registry seam 같은 narrower factory seam에 연결하는 얇은 조합 레이어다.
- 따라서 현재 단계에서는 `server_core`나 별도 중립 패키지로 승격하지 않고, 해당 실행 파일이 있는 `server/`, `gateway/`, `tools/` 트리 안에 둔다.
- helper target은 아래 조건을 모두 만족할 때만 상위 패키지로 이동을 검토한다.
  - 둘 이상의 실행 파일이 동일한 helper 구현을 그대로 공유한다.
  - helper가 앱/툴 로컬 설정, CLI, 수명주기 의미를 더 이상 직접 해석하지 않는다.
  - 연결 대상 backend seam이 installable package/factory contract로 먼저 안정화돼 있다.
- 반대로 단일 실행 파일의 composition root를 감추는 목적이라면, 이름이 다소 일반적이어도 app-local helper로 남기는 편이 ownership 경계를 더 분명하게 유지한다.

## 3. 실행 흐름
1. 실행 환경/인자에서 설정을 읽고 `ServiceRegistry`를 초기화한 뒤 generic DB connection/worker seam, Redis, Write-behind, TaskScheduler를 등록한다.
2. `core::net::Session`은 wire decoder로 opcode를 구문 분석한 뒤 Dispatcher에 넘기고, Dispatcher는 ServiceRegistry에서 필요한 핸들러를 찾는다.
3. 백그라운드 DB 작업은 `DbWorkerPool`이 generic `IUnitOfWork` 경계를 열어 Worker 스레드에서 실행하고, 도메인 repository 접근은 `server/storage/*` 계층이 담당한다.
4. `TaskScheduler`는 health check, presence TTL 정리, metrics 플러시, registry heartbeat 작업을 주기적으로 수행한다.
5. 프로세스 lifecycle은 `AppHost` phase 전이로 표준화되며, readiness/health와 분리되어 운영 상태 추적에 사용된다.

## 4. 향후 확장 항목
| 항목 | 상태 | 메모 |
| --- | --- | --- |
| Hive/Connection 재사용 | Gateway는 `core::net::TransportConnection`, Server는 `core::net::Session` 중심으로 사용 | 필요 시 공통 수명주기 규칙을 추출 가능 |
| 인증 플러그인 | `auth::IAuthenticator` 인터페이스로 구현, 기본은 NoopAuthenticator | 외부 OAuth 연동 시 구현 교체 |
| 스크립팅 훅 | Lua/plugin extensibility는 이미 `core::plugin` + `core::scripting` 기반 stable mechanism capability로 존재 | 추가 소비자(gateway/wb_worker) 확대와 service-specific ABI 안정화는 후속 검증 |
| ECS/플러그인 | 채팅 외 모듈을 위한 Entity 시스템은 backlog에 남겨둠 | 필요 시 별도 설계/작업 메모로 후속 추적 |
| 관측성 표준화 | `/metrics` + structured log를 기본 제공, OpenTelemetry 추가 검토 | server_app에 우선 적용 후 Gateway/LB로 확장 |

## 5. 테스트 전략
- `core/tests/`에 TaskScheduler, DbWorkerPool 등 핵심 컴포넌트의 단위테스트를 추가해 CI에서 돌린다.
- gRPC/TCP 엔드투엔드 테스트(`tests/integration/`)를 확장해 로그인→채팅→세션 종료까지 자동화한다.
- PowerShell/Bash smoke 스크립트(`scripts/smoke_*.ps1`)로 Redis·Write-behind 경로를 CI에 포함한다.

## 6. 참고 문서
- 저장소 개요: `README.md`
- 운영 토폴로지/다이어그램: `docs/ops/gateway-and-lb.md`, `docs/ops/architecture-diagrams.md`
- Gateway & HAProxy 운영: `docs/ops/gateway-and-lb.md`
- Redis/Write-behind 전략: `docs/db/redis-strategy.md`, `docs/db/write-behind.md`
