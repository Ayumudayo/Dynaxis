# 코어(Core) 설계 노트

`server_core`는 Dynaxis의 공용 실행 플랫폼이다. 목적은 채팅 서버 구현을 통째로 옮기는 것이 아니라, 여러 바이너리가 공통으로 써야 하는 runtime 규약과 reusable contract를 한곳에 두는 것이다.

이 문서는 current-state 요약본이다.

- 왜 이런 구조가 되었는지는 `docs/core-architecture-rationale.md`
- 공개 API 범위와 안정성 분류는 `docs/core-api-boundary.md`, `docs/core-api/overview.md`

## 1. core가 소유하는 것

- 프로세스 lifecycle, readiness, dependency 상태, admin HTTP 같은 공통 runtime host 규약
- generic transport substrate와 packet dispatch policy
- worker queue, 스케줄러, 메모리 풀 같은 bounded execution primitive
- process-wide observability substrate(metrics, logging, trace, build info)
- domain-neutral discovery / storage execution / realtime / world orchestration contract
- service-neutral plugin / Lua mechanism layer

## 2. core가 소유하지 않는 것

- 채팅 도메인 규칙과 room/message/user repository 계층
- gateway sticky `SessionDirectory`
- concrete Redis / Consul / Postgres adapter 전략
- chat hook ABI, chat Lua bindings 같은 service-specific extensibility contract
- 실행 파일별 composition root와 CLI/운영 문맥
- Kubernetes / AWS / Docker SDK 직접 호출

핵심 기준은 단순하다.

- 여러 consumer가 공통으로 의존해야 하는 규약이면 core
- 특정 앱이나 배포 방식의 의미라면 core 밖

## 3. 레이어 지도

| 레이어 | 주요 경로 | core가 소유하는 이유 |
|---|---|---|
| Runtime Host | `core/app/*` | `server_app`, `gateway_app`, `tools`가 같은 lifecycle/readiness/shutdown 규약을 써야 하기 때문 |
| Networking | `core/net/*`, `core/protocol/*` | generic transport substrate와 packet dispatch policy는 재사용 가능하지만, 앱별 세션 의미는 분리해야 하기 때문 |
| Execution / Memory | `core/concurrent/*`, `core/memory/*` | queue/scheduler/pool의 bounded semantics를 공용 규약으로 유지해야 하기 때문 |
| Observability | `core/metrics/*`, `core/runtime_metrics.hpp`, `core/util/log.hpp`, `core/trace/*` | 운영 표준화와 장애 분석 속도를 프로세스 공통 규약으로 강제해야 하기 때문 |
| Discovery / Shared State | `core/discovery/*`, underlying `core/state/*` | shared instance record/selector/backend seam은 공용 개념이지만 concrete adapter는 아직 앱/배포 전략이기 때문 |
| Storage Execution | `core/storage_execution/*`, underlying `core/storage/*` | generic transaction/worker/retry seam은 공용 계약이지만 domain repository와 concrete adapter는 app-owned여야 하기 때문 |
| Extensibility | `core/plugin/*`, `core/scripting/*` | service-neutral mechanism은 공용 platform capability지만 도메인 ABI는 분리해야 하기 때문 |
| Realtime / Worlds | `core/realtime/*`, `core/worlds/*` | engine-neutral capability와 control-plane vocabulary는 공용 계약으로 유지하되 provider/game rules는 바깥에 두기 위해 |
| Utilities / Leaf Services | `core/security/*`, `core/compression/*`, `core/util/paths.hpp` | 여러 바이너리에서 반복 구현할 이유가 없는 공통 leaf capability이기 때문 |

## 4. 가장 중요한 경계 결정

### 4.1 `Connection`은 stable이고 `Session`은 internal이다

- `Connection`
  - generic TCP transport
  - async read/write
  - FIFO send queue
  - bounded backpressure
- `Session`
  - fixed packet header
  - heartbeat/read/write timeout
  - `Dispatcher`
  - `SessionOptions`
  - `ConnectionRuntimeState`

즉 `Connection`은 reusable substrate이고, `Session`은 현재 server packet-session implementation이다.

canonical consumer 이름은 `Listener` / `Connection`이다. `SessionListener`, `TransportListener`, `TransportConnection` 같은 이름은 compatibility 또는 내부 편의 이름으로만 본다.

### 4.2 `EngineContext`는 기준이고 `ServiceRegistry`는 compatibility bridge다

- `EngineContext`
  - instance-scoped typed registry
  - 새 composition model의 기준
- `ServiceRegistry`
  - process-global bridge
  - legacy / plugin / shared-library 경로 호환용

이중 구조는 이상형이 아니라, multi-runtime 격리와 기존 global lookup 소비자를 동시에 만족시키기 위한 현실적 타협이다.

### 4.3 public contract와 underlying implementation을 의도적으로 분리한다

현재 대표 사례:

- `server/core/discovery/**` -> canonical public path
- `server/core/state/**` -> underlying/internal implementation path
- `server/core/storage_execution/**` -> canonical public path
- `server/core/storage/**` -> underlying/internal implementation path

이 패턴의 목적은 "public 이름은 빨리 고정하고, 내부 구현 소유권은 계속 조정 가능하게 두는 것"이다.

### 4.4 extensibility에서는 mechanism만 core가 소유한다

stable:

- `server/core/plugin/*`
- `server/core/scripting/*`

app-owned/transitional:

- chat hook ABI
- chat plugin chain policy
- chat Lua bindings

즉 core는 "확장 메커니즘"을 제공하고, 특정 서비스의 ABI 의미는 core 밖에 둔다.

### 4.5 realtime/worlds surface는 구현보다 contract를 우선한다

`core/realtime/**`와 `core/worlds/**`는 concrete orchestrator나 game rule이 아니라 다음을 public으로 제공한다.

- authoritative fixed-step capability
- bind / delivery / quality policy contract
- desired-vs-observed topology vocabulary
- drain / transfer / migration / provider adapter status evaluation

이 표면은 대부분 data struct + pure evaluation helper 중심으로 유지된다.

### 4.6 reusable-looking helper target도 composition root면 core 밖에 둔다

`server_app_backends`, `gateway_backends`, `admin_app_backends`, `wb_common_redis_factory` 같은 타깃은 이름만 보면 재사용 가능한 module처럼 보이지만, 실제로는 실행 파일별 composition helper다. 이들은 app/tool-local 설정과 수명주기 문맥을 해석하므로 core contract가 아니라 app-local ownership이 맞다.

## 5. 현재 조립 흐름

1. 실행 파일이 `EngineBuilder` / `EngineRuntime` 기준으로 lifecycle, dependency, admin HTTP를 조립한다.
2. 앱별 bootstrap이 listener, routes, worker, backend adapter 같은 app-local behavior를 붙인다.
3. network ingress는 `Hive` / `Listener` / `Connection` 또는 internal `Session`을 통해 처리된다.
4. handler execution은 `Dispatcher`와 `OpcodePolicy`가 `inline / worker / room_strand` policy로 분기한다.
5. background work는 `JobQueue`, `ThreadManager`, `TaskScheduler`, `DbWorkerPool` 같은 bounded execution seam으로 흘러간다.
6. observability는 `runtime_metrics`, `metrics`, `MetricsHttpServer`, async logger, trace context를 통해 공통 노출된다.

## 6. 현재 문서 읽기 순서

1. `docs/core-design.md`
2. `docs/core-architecture-rationale.md`
3. `docs/core-api/overview.md`
4. `docs/core-api-boundary.md`
5. 필요한 세부 영역 문서:
   - `docs/core-api/net.md`
   - `docs/core-api/metrics-and-lifecycle.md`
   - `docs/core-api/storage.md`
   - `docs/core-api/discovery.md`
   - `docs/core-api/extensions.md`
   - `docs/core-api/realtime.md`
   - `docs/core-api/worlds-*.md`
