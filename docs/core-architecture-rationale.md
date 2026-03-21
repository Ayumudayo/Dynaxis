# 코어 아키텍처 배경 설명

이 문서는 `server_core`가 왜 현재 구조를 가지는지, 각 계층이 왜 그렇게 분리되었는지, 그리고 어떤 제약 때문에 그 설계가 사실상 필요했는지를 설명한다.

핵심 원칙은 단순하다.

- `core/`는 "재사용 가능한 실행 플랫폼"이어야 한다.
- `server/`, `gateway/`, `tools/`는 그 플랫폼 위에서 조립되는 앱이어야 한다.
- 운영 규약, 성능 한계, 공용 데이터 계약은 core에 둔다.
- 채팅 규칙, sticky routing, 저장소 스키마, provider SDK 결합 같은 앱/배포 특화 로직은 core 밖에 둔다.

이 문서는 기존의 요약형 설계 문서인 `docs/core-design.md`를 대체하지 않는다.

- `docs/core-design.md`: 현재 ownership과 layering의 압축된 지도
- `docs/core-api/*.md`: 공개 surface와 호환성 계약
- 이 문서: 왜 그 구조가 되었는지에 대한 설계 이유와 trade-off

현재 상태를 설명하는 기준은 다음 셋이다.

1. `core/` 실제 헤더와 구현
2. `docs/core-api-boundary.md`
3. `docs/core-api/*.md`의 current-state 계약

다른 오래된 노트와 충돌하면 현재 코드를 우선한다.

## 1. 한 문장으로 보는 `server_core`

`server_core`는 네트워크 세션 처리, 런타임 조립, 관측성, 공용 상태/저장소 seam, 확장 메커니즘, realtime/world orchestration contract를 담은 installable C++20 runtime substrate다.

중요한 점은 이것이 "범용 게임 엔진"도 아니고 "채팅 서버 구현"도 아니라는 점이다.

- 범용 엔진이 아닌 이유:
  - 렌더링, 에셋, 도메인 규칙을 소유하지 않는다.
- 단순 공용 유틸 라이브러리도 아닌 이유:
  - 프로세스 lifecycle, readiness, metrics, network backpressure, extension reload, topology contracts처럼 실행 중 의미를 갖는 정책을 포함한다.
- 채팅 서버 구현이 아닌 이유:
  - 채팅 hook ABI, room storage schema, gateway sticky session directory는 core 밖에 남겨 둔다.

즉, core는 "공용 실행 플랫폼"이지 "최종 제품"이 아니다.

## 2. 이 구조를 강제한 현실적 압력

### 2.1 여러 바이너리가 같은 런타임 규약을 공유해야 했다

이 리포에는 역할이 다른 프로세스가 공존한다.

- `server_app`: 패킷 세션과 비즈니스 처리
- `gateway_app`: 엣지 수락과 backend 브리지
- `wb_worker` 및 기타 tool: storage/metrics/lifecycle 중심의 보조 프로세스

이들이 각각 shutdown, readiness, metrics, logging, registry wiring을 제각각 구현하면 운영이 곧바로 깨진다.

- 어떤 프로세스는 `SIGTERM`에 즉시 내려가고
- 어떤 프로세스는 `ready=false`를 늦게 내리고
- 어떤 프로세스는 `/metrics`가 느려지고
- 어떤 프로세스는 전역 서비스 정리를 잘못해서 다른 런타임까지 지우게 된다

그래서 `core/app`, `core/metrics`, `core/runtime_metrics`, `core/util/log` 같은 "운영 규약 계층"이 core 안으로 들어왔다.

### 2.2 core는 앱보다 오래 살아남는 계약이어야 했다

`core/CMakeLists.txt`는 `server_core`를 설치 가능한 패키지로 export한다. 이건 단순 빌드 편의가 아니라 설계 선언이다.

- core는 monorepo 내부 include 모음이 아니라
- 외부 consumer도 링크 가능한 package surface여야 하고
- 따라서 public contract와 internal implementation을 분리해야 한다

이 때문에 `Stable / Transitional / Internal` 경계가 강하게 필요해졌다.

- `docs/core-api-boundary.md`
- `core/include/server/core/api/version.hpp`
- `docs/core-api/compatibility-policy.md`

즉, "일단 public header로 열어 두고 나중에 정리"하는 방식으로는 더 이상 유지할 수 없었다.

### 2.3 성능 문제보다 더 위험한 것은 경계 붕괴였다

초기 서버 코드는 보통 성능 때문에만 substrate를 만든다. 이 저장소는 그 단계를 지나 "경계 보존"이 더 중요한 단계로 갔다.

예를 들어:

- `Connection`은 generic TCP transport다.
- `Session`은 framed packet, heartbeat, opcode dispatch, `SessionOptions`, `ConnectionRuntimeState`에 결합된 server-specific packet session이다.

둘을 합치면 단기적으로는 편해 보인다. 하지만 그렇게 되면:

- gateway가 generic transport를 재사용하기 어려워지고
- public API는 server-specific semantics에 오염되고
- stable contract를 만들 수도 없게 된다

그래서 지금 구조는 일부 중복을 감수하고라도 경계를 보존한다.

### 2.4 "운영 가능성"이 1급 요구사항이었다

이 core는 단순히 동작하면 끝이 아니다. 운영 관측 없이 문제를 재현할 수 없는 환경을 상정한다.

그래서 core는 처음부터 다음을 포함한다.

- process-wide counters: `runtime_metrics`
- lightweight exporter registry: `metrics`
- dedicated admin HTTP: `MetricsHttpServer`
- async logging + masking + trace correlation: `util/log`, `trace/context`
- build info injection: `build_info`, `metrics/build_info.hpp`

이 계층은 비즈니스 기능이 아니라 운영 복구 속도를 위한 설계다.

### 2.5 provider/runtime 세부 구현을 public contract에 박아 넣을 수 없었다

`realtime/**`, `worlds/**`, `discovery/**`, `storage_execution/**`는 대체로 "데이터 계약 + 평가 함수 + 좁은 interface" 위주다.

그 이유는 분명하다.

- Kubernetes를 직접 core에 묶으면 provider-neutral contract가 사라진다.
- Redis/Postgres concrete adapter를 stable로 올리면 교체 여지가 줄어든다.
- chat repository DTO를 core로 올리면 core가 도메인에 종속된다.

그래서 core는 "구성 가능한 seam"을 public으로 만들고, concrete wiring은 앱 쪽에 남긴다.

### 2.6 core는 정적 라이브러리이면서도 package처럼 다뤄져야 했다

`server_core`는 정적 라이브러리지만, 설계적으로는 installable package다.

- `core/CMakeLists.txt`는 export target과 package config를 만든다
- public header는 install 대상으로 취급된다
- API 버전은 `core/include/server/core/api/version.hpp`가 기준이다

이 결정의 의미는 분명하다.

- "같은 repo 안에서만 쓰면 된다"는 가정이 더 이상 성립하지 않는다
- internal convenience header를 public처럼 열어둘 수 없다
- 문서, boundary inventory, installed-consumer proof가 모두 같은 truth를 봐야 한다

즉 core의 구조는 빌드 산출물 형태까지 포함해 package-first로 설계되었다.

## 3. 가장 중요한 구조: 경계 아키텍처

### 3.1 core는 한 덩어리 라이브러리처럼 보이지만, 실제로는 세 종류의 surface를 가진다

1. public stable surface
2. public transitional surface
3. internal implementation surface

이 분리는 문서용이 아니라 실제 진화 전략이다.

#### Stable

오래 유지해야 하는 contract다.

예:

- `server/core/app/*`
- `server/core/metrics/*`
- `server/core/realtime/*`
- `server/core/worlds/*`
- `server/core/storage_execution/*`

#### Transitional

지금은 public처럼 보이지만, 의미가 아직 service-specific consumer와 함께 다듬어지는 표면이다.

현재 대표적인 예는 chat-specific extensibility consumer 쪽이며, mechanism 자체는 stable로 승격되었다.

#### Internal

공유 구현이지만 외부 contract로 묶기엔 아직 coupling이 큰 표면이다.

예:

- `server/core/net/session.hpp`
- `server/core/net/acceptor.hpp`
- `server/core/state/redis_backend_factory.hpp`
- `server/core/net/rudp/*`

### 3.2 이 경계는 왜 필요한가

이 경계가 없으면 세 가지가 동시에 망가진다.

- package versioning 기준이 없어진다
- external consumer 검증이 불가능해진다
- 앱별 편의용 구현이 영구 public ABI가 된다

특히 `docs/core-api/overview.md`를 보면 `storage_execution`과 `discovery`는 canonical stable 이름을 갖지만, 실제 구현은 더 아래 `storage/*`, `state/*`에 남아 있다. 이것은 우회가 아니라 의도적인 facade 패턴이다.

핵심 의도는 다음과 같다.

- public contract 이름은 일찍 고정한다
- 구현 소유권은 내부에서 계속 바꿀 수 있게 둔다
- consumer는 canonical path만 보게 만든다

즉, "public 이름의 안정성"과 "내부 구현의 이동 가능성"을 동시에 확보하려는 구조다.

### 3.3 one-way dependency는 이 프로젝트의 가장 강한 규칙이다

`core/`가 `server/`나 `gateway/`를 보면 안 된다. 이건 취향이 아니라 생존 조건이다.

왜냐하면 core가 상위 앱을 보기 시작하면:

- core package export가 무의미해지고
- 앱 분리/재조합이 불가능해지고
- 테스트가 monorepo include leakage에 의존하게 된다

그래서 현재 구조는 다음을 강하게 유지한다.

- `server/`, `gateway/`, `tools/` -> `core/`
- `core/` -X-> `server/`, `gateway/`

### 3.4 canonical 이름과 compatibility alias를 왜 분리하는가

이 저장소는 "현재 기준 이름"과 "이전 단계에서 남겨 둔 alias"를 동시에 갖는다.

canonical public 이름:

- `Listener`
- `Connection`
- `server/core/discovery/**`
- `server/core/storage_execution/**`
- `server/core/realtime/**`
- `server/core/worlds/**`

compatibility 또는 underlying 이름:

- `SessionListener`
- `TransportListener`
- `TransportConnection`
- `server/core/state/**`
- `server/core/storage/**`

이 분리를 유지하는 이유는 두 가지다.

1. 새 consumer에게는 현재 기준 이름을 고정해 줘야 한다.
2. 기존 repo 코드와 staged migration path는 하루아침에 끊을 수 없다.

즉 alias를 남기는 이유는 "옛 이름도 계속 public이다"가 아니라, "migration 비용을 낮추되 canonical contract는 새 이름으로 고정한다"이다.

### 3.5 일부 public surface가 "얇은 facade"인 것은 의도적이다

겉으로 보기엔 `discovery/*.hpp`나 `storage_execution/*.hpp`가 너무 얇아 보일 수 있다. 하지만 이 얇음 자체가 설계 포인트다.

이 facade들은 다음 역할을 한다.

- consumer에게 canonical public 이름을 제공한다
- 내부 구현 경로(`state/*`, `storage/*`)를 바로 public ABI로 굳히지 않는다
- 구현 ownership과 factory 전략을 나중에 더 좁게 재편할 여지를 남긴다

즉, public API는 항상 "큰 구현"일 필요가 없다. 때로는 "좋은 경계 이름"이 더 중요하다.

### 3.6 왜 조합 헬퍼 타깃은 core가 아닌 app-local ownership으로 남는가

다음 타깃들은 얼핏 보면 reusable engine module처럼 보인다.

- `server_app_backends`
- `gateway_backends`
- `admin_app_backends`
- `wb_common_redis_factory`

하지만 이들은 reusable engine contract가 아니라 composition helper다.

이 타깃들이 app/tool-local로 남는 이유:

- 프로세스별 설정 해석과 수명주기 문맥을 직접 안다
- narrower factory seam을 조합하는 composition root 역할을 한다
- domain/runtime ownership보다 "어느 실행 파일을 어떻게 조립하느냐"가 더 중요하다

즉, 재사용 가능해 보인다는 이유만으로 core로 승격하면 안 된다. composition helper를 core에 넣는 순간 engine contract와 app wiring의 경계가 다시 흐려진다.

## 4. 런타임 조립 아키텍처: `core/app`

이 계층은 현재 core에서 가장 중요한 "조립 seam"이다.

### 4.1 왜 `AppHost`가 필요한가

`AppHost`는 큰 프레임워크가 아니라 "공통 프로세스 제어 규칙"을 캡슐화한 작은 호스트다.

소유하는 것은 제한적이다.

- stop 요청
- lifecycle phase
- health / readiness
- declared dependencies
- admin HTTP
- shutdown step registry
- signal hookup

이렇게 작게 유지한 이유는 두 가지다.

1. 공통화해야 하는 운영 규약만 강제하려고
2. listener/routes/workers 같은 앱별 조립은 여전히 앱 쪽에 남기려고

즉, `AppHost`는 composition root를 완전히 삼키는 객체가 아니다. 오히려 "공통이어야만 하는 것만 강제하는 최소 공통 분모"다.

### 4.2 왜 `EngineContext`와 `ServiceRegistry`가 둘 다 존재하는가

이중 구조는 타협의 결과다.

- `EngineContext`
  - instance-scoped
  - local typed registry
  - 새 구조의 기준
- `ServiceRegistry`
  - process-global compatibility bridge
  - 기존 코드와 plugin/shared-library 경계 호환용

이 프로젝트는 이미 전역 registry 사용 흔적이 있는 상태에서 embeddability를 강화해야 했다. 전역 registry를 즉시 없애면 기존 bootstrap과 plugin 소비자가 다 깨진다. 그렇다고 그대로 두면 runtime instance 간 격리가 불가능하다.

그래서 나온 해법이 `EngineRuntime`이다.

- context에는 instance-local service를 둔다
- 필요할 때만 global registry로 bridge한다
- bridge entry는 owner token으로 추적한다
- `clear_global_services()`는 자기 runtime이 올린 bridge만 지운다

즉, 이것은 "전역 singleton 제거 이전의 안전한 과도기"이자, 동시에 multi-runtime 환경을 깨지 않게 만드는 장치다.

### 4.3 왜 owner-tagged bridge가 필요한가

`ServiceRegistry` 구현은 하나의 타입 키에 여러 owner entry를 보관하고, `get()`은 가장 마지막 entry를 읽는다. 이 구조는 겉보기엔 unusual하지만 이유가 명확하다.

- shared library / plugin이 main process의 registry를 재사용해야 한다
- legacy 코드 경로는 여전히 global lookup을 기대한다
- 하지만 하나의 runtime이 teardown될 때 다른 runtime의 bridge를 지우면 안 된다

그래서:

- registry address는 환경 변수로 공유한다
- bridge entry는 owner token으로 추적한다
- runtime 단위 clear가 가능해진다

이는 "깔끔한 DI 컨테이너"라기보다 "레거시 호환성과 embeddability 사이의 현실적 절충"이다.

### 4.4 왜 `EngineBuilder`까지 있는가

`EngineBuilder`는 기능적으로 대단하지 않다. 하지만 중요한 역할이 있다.

- bootstrapping phase 초기값을 표준화
- dependency declaration 순서를 표준화
- optional admin HTTP 기동을 같은 방식으로 수행

즉, builder의 목적은 유연성보다 "조립 모양의 일관성"이다. 이 프로젝트에서 bootstrap drift는 버그 원인이었기 때문에, 최소한의 선언형 entrypoint가 필요했다.

### 4.5 `termination_signals`가 왜 poll-friendly한가

시그널 핸들러 안에서는 lock, allocation, logging을 안전하게 할 수 없다. 그래서 이 계층은 시그널 핸들러에서 `sig_atomic_t` 플래그만 세우고 실제 shutdown은 루프 바깥에서 수행한다.

이 결정은 다음을 가능하게 한다.

- Asio 루프 기반 프로세스
- non-Asio worker/control-plane 루프
- 테스트 환경

모두가 같은 종료 의도를 공유할 수 있다.

## 5. 전송/세션 아키텍처: `core/net` + `core/protocol`

이 계층은 "generic transport substrate"와 "server packet session"을 의도적으로 분리한다.

### 5.1 `Hive`: 왜 아직도 별도 래퍼인가

`Hive`는 사실상 `io_context` + `work_guard` wrapper다. 작지만 중요한 이유가 있다.

- shared `io_context`의 run/stop 책임을 한 곳에 모은다
- work guard로 조기 `run()` 반환을 막는다
- 다중 모듈이 같은 event loop를 쓸 때 lifecycle race를 줄인다

즉, `Hive`의 목적은 추상화 자체가 아니라 "수명주기 실수를 중앙에서 줄이는 것"이다.

### 5.2 `Listener`: 왜 accept loop만 소유하는가

`Listener`는 intentionally thin하다.

- bind / listen / async_accept
- `connection_factory`
- `on_accept`, `on_error`

여기서 중요한 것은 `connection_factory` 주입이다.

- gateway는 backend bridge 또는 edge connection을 만들 수 있고
- server는 packet-aware connection/session 계층으로 넘어갈 수 있다

리스너가 protocol semantics까지 알게 만들지 않은 이유는 "accept loop는 공용, 세션 의미는 앱별"이기 때문이다.

### 5.3 `Connection`: 왜 generic transport가 stable인가

`Connection`은 다음만 약속한다.

- async read loop
- FIFO write queue
- bounded send queue
- strand-based serialization
- stop idempotency

이 객체는 payload framing을 모른다. 이게 핵심이다.

generic TCP substrate가 stable인 이유:

- gateway와 다른 consumer가 그대로 재사용 가능하다
- public contract로 유지해도 domain leakage가 적다
- backpressure와 lifecycle semantics는 transport 공통 요구사항이다

반대로 `Session`은 stable이 아니다.

### 5.4 `Session`이 internal인 이유

`Session`은 다음에 강하게 결합되어 있다.

- fixed packet header (`PacketHeader`)
- `Dispatcher`
- `SessionOptions`
- `ConnectionRuntimeState`
- heartbeat / read timeout / write timeout
- `MSG_HELLO`, `MSG_PING`, `MSG_ERR`

즉, 이것은 generic transport가 아니라 "이 저장소의 packet-session implementation"이다.

이걸 stable로 올리면 문제가 생긴다.

- protocol semantics가 core public ABI가 된다
- gateway나 다른 consumer가 불필요한 TCP packet session 규약을 같이 끌어안게 된다
- future transport/session refactor가 어려워진다

따라서 현재 구조는 정당하다.

- `Connection`: reusable transport primitive, stable
- `Session`: current server packet session, internal

### 5.5 왜 `Dispatcher`가 비즈니스 로직을 전혀 모르는가

`Dispatcher`는 opcode를 handler로 연결하고, 실행 위치와 transport/state policy만 검사한다.

여기서 중요한 것은 `OpcodePolicy`다.

- required session state
- processing place
- allowed transport
- delivery class

이 메타데이터를 handler table 옆에 두는 이유는 "메시지 처리 규칙"을 단순 switch-case가 아니라 transport/runtime policy까지 포함한 계약으로 만들기 위해서다.

또한 dispatcher는 예외를 세션 단위로 흡수한다.

- handler exception이 프로세스를 죽이지 않는다
- metrics와 log로 failure가 남는다
- `SERVER_BUSY` 또는 `INTERNAL_ERROR`처럼 경계 친화적인 오류로 변환된다

즉, dispatcher는 단순 라우터가 아니라 "비즈니스 코드와 runtime failure domain 사이의 방화벽"이다.

### 5.6 왜 `processing_place`가 필요한가

모든 handler를 같은 스레드/같은 방식으로 실행하면 쉽다. 하지만 그렇게 하면:

- room-serialized work가 섞이고
- expensive work가 I/O loop를 막고
- control flow가 handler마다 제각각이 된다

그래서 `OpcodePolicy::processing_place`가 생겼다.

- `kInline`
- `kWorker`
- `kRoomStrand`

이는 "성능 튜닝용 옵션"이 아니라 "실행 위치를 계약으로 명시하는 장치"다.

### 5.7 왜 backpressure가 여러 층에 나뉘어 있는가

이 프로젝트는 의도적으로 단일 queue만 믿지 않는다.

- `Connection`은 generic send-queue max를 가진다
- `Session`은 `SessionOptions::send_queue_max`와 `queue_budget` helper를 쓴다
- `JobQueue`는 bounded mode에서 block 또는 reject semantics를 가진다
- gateway는 token bucket / retry budget / circuit breaker를 추가로 쓴다

이런 다층 구조가 필요한 이유는 overload 양상이 서로 다르기 때문이다.

- transport queue 폭주
- worker queue 폭주
- backend retry 폭주
- ingress burst 폭주

하나의 universal limiter로는 각각의 failure mode를 설명하기 어렵다.

### 5.8 `RUDP`가 internal인 이유

`core/net/rudp/*`는 구현은 존재하지만 boundary상 internal이다.

이것은 미완성이라서 숨긴 것이 아니라, 아직 contract를 고정할 시점이 아니기 때문이다.

현 상태에서 RUDP는:

- canary/rollout 전개 대상
- fallback semantics와 inflight policy가 계속 조정될 가능성이 있음
- gateway integration semantics가 더 중요함

반면 public realtime surface는 이미 별도로 존재한다.

- `realtime/direct_bind.hpp`
- `realtime/transport_policy.hpp`
- `realtime/direct_delivery.hpp`
- `realtime/transport_quality.hpp`

즉, core는 "public transport policy contract"와 "internal RUDP engine implementation"을 분리해 뒀다. 이것이 맞는 설계다.

### 5.9 `protocol`이 생성 코드 중심인 이유

`system_opcodes.hpp`는 generated file이다. 이것도 중요한 구조 결정이다.

- opcode 상수
- human-readable name
- default policy

를 한 source of truth에서 만든다.

이렇게 해야만 다음이 동시에 가능하다.

- 중복 opcode 방지
- 문서/코드/검증의 동기화
- transport/delivery policy drift 방지

즉, protocol generation은 편의 기능이 아니라 "계약 일관성 유지 장치"다.

## 6. 실행/메모리 아키텍처: `core/concurrent`, `core/memory`

### 6.1 왜 `JobQueue`는 단순 mutex + condition_variable인가

이 계층은 lock-free queue를 쓰지 않는다. 의도적이다.

이 프로젝트에서 더 중요한 것은:

- FIFO semantics가 명확할 것
- stop 시그널이 명확할 것
- bounded queue일 때 pressure behavior가 예측 가능할 것

`JobQueue`는 이 세 가지를 명확히 제공한다.

- `Push()`: bounded면 block
- `TryPush()`: bounded면 reject
- `Pop()`: stop 이후 empty면 null job

즉, 이 큐는 최대 성능보다 "운영 가능한 압력 모델"을 우선한다.

### 6.2 왜 `ThreadManager`는 고정 worker-pool인가

작업마다 thread를 만드는 방식은 간단하지만, 서버 런타임에서 다음 문제가 있다.

- 생성/파괴 오버헤드
- shutdown synchronization 복잡도
- saturation 시 scheduler thrash

그래서 `ThreadManager`는 오직 하나의 역할만 갖는다.

- `JobQueue`를 소비하는 고정 worker-pool

이 작은 wrapper가 필요한 이유는 "thread lifecycle을 app code에서 반복 작성하지 않게 하기 위해서"다.

### 6.3 왜 `TaskScheduler`는 자체 스레드를 소유하지 않는가

`TaskScheduler`는 poll 기반이다. 처음 보면 불편해 보인다. 하지만 이 선택은 의도적이다.

장점:

- 호출자가 tick budget을 제어할 수 있다
- 앱마다 기존 event loop와 결합하기 쉽다
- shutdown semantics가 단순하다
- 별도 timer thread가 없어 observability가 쉽다

단점:

- 누군가는 규칙적으로 `poll()`을 호출해야 한다

이 프로젝트는 그 단점을 감수하고도 "실행 제어권을 bootstrap/runtime이 유지"하는 편을 선택했다.

### 6.4 왜 `MemoryPool`이 실패를 `nullptr`로 노출하는가

많은 pool 구현은 필요하면 자동 확장한다. 여기서는 기본적으로 그렇지 않다.

이유:

- 메모리 상한을 운영적으로 강제할 수 있어야 한다
- burst 시 무제한 확장보다 명시적 실패가 낫다
- 네트워크 패킷 경로에서는 worst-case 메모리 footprint가 중요하다

그래서 `Acquire()` 실패는 진짜 신호다. 호출자는 이 실패를 처리해야 한다.

### 6.5 왜 `BufferManager`가 따로 있는가

`MemoryPool`은 raw block allocator다. `BufferManager`는 그 위에 RAII를 얹는다.

즉 역할이 분리된다.

- `MemoryPool`: bounded raw allocation policy
- `BufferManager`: unique_ptr + custom deleter 기반 safe ownership

이 분리는 단순하지만 매우 중요하다. 그래야 hot path는 싸고, 호출자는 lifetime bug를 줄일 수 있다.

## 7. 관측성 아키텍처: metrics, runtime_metrics, logging, trace

이 프로젝트의 core는 "운영 계층도 플랫폼의 일부"라고 본다.

### 7.1 왜 `runtime_metrics`는 process-global atomic 집합인가

`runtime_metrics`는 아주 전통적인 방식이다.

- process-global
- atomic counter/gauge
- snapshot 시점에만 집계

이것을 택한 이유는 hot path 비용 때문이다.

- packet receive
- dispatch
- send queue drop
- HTTP reject
- RUDP fallback

이 경로에서 allocator-heavy metrics backend를 직접 건드리면 오버헤드가 커진다. 그래서 기록은 싸게 하고, export 시점에 텍스트로 바꾼다.

이 구조의 trade-off도 분명하다.

- instance-scoped telemetry에는 덜 적합하다

그래서 core는 별도로 `EngineRuntime::snapshot()`을 제공한다.

- instance state는 runtime snapshot
- process-wide operational counters는 runtime_metrics

둘을 분리한 것이 포인트다.

### 7.2 왜 `metrics::metrics`와 `runtime_metrics`가 둘 다 존재하는가

둘은 목적이 다르다.

- `runtime_metrics`
  - core hot path용 fixed operational series
  - low overhead
  - schema가 거의 고정
- `metrics::metrics`
  - named counter/gauge/histogram registry
  - 서비스/도구가 추가 메트릭을 쉽게 노출
  - Prometheus text export backend

즉 하나는 substrate counter plane이고, 다른 하나는 extensible metric registry다.

### 7.3 왜 `MetricsHttpServer`가 별도 io_context/thread를 가지는가

운영 포트는 데이터 포트를 방해하면 안 된다. 그래서 이 서버는:

- 별도 thread
- 별도 `io_context`
- small HTTP surface
- hardening env knobs

을 가진다.

핵심 이유:

- `/metrics` scrape가 session I/O를 막으면 안 된다
- health/readiness는 가볍게 응답해야 한다
- bad request / oversize / auth failure도 관측 가능해야 한다

즉 exporter는 "있으면 좋은 편의 기능"이 아니라 "데이터 경로를 오염시키지 않는 별도 운영 plane"이다.

### 7.4 왜 logging이 비동기 queue 기반인가

로그는 필요하지만, request path를 멈추면 안 된다. `util/log.cpp`는 그래서 다음을 한다.

- producer는 queue push만 수행
- background worker가 실제 stderr flush
- overflow policy는 env var로 조정
- recent buffer는 `/logs`와 local debugging에 사용
- 민감 필드는 mask
- log schema completeness까지 metrics로 기록

이건 단순 printf wrapper가 아니다. "관측 품질"과 "경로 지연"을 동시에 관리하는 계층이다.

### 7.5 trace context가 왜 별도 전역이 아닌 thread-local인가

trace는 request-local 문맥이어야 한다. 그래서:

- config는 process-global
- active trace/correlation context는 thread-local
- `ScopedContext`가 RAII로 교체

이 선택 덕분에 로그 formatting은 현재 문맥을 자연스럽게 읽을 수 있고, sampling도 deterministic seed 기반으로 제어할 수 있다.

## 8. 저장소 실행 아키텍처: `storage_execution` vs `storage`

이 영역은 core boundary 설계의 대표 사례다.

### 8.1 왜 `storage_execution` public surface가 얇은 alias 계층인가

`storage_execution/*.hpp`는 거의 wrapper/alias처럼 보인다. 이것은 불완전함이 아니라 의도다.

public contract로 필요한 것은 많지 않았다.

- transaction boundary
- connection pool seam
- async DB worker
- retry/backoff policy

반면 공개하면 안 되는 것은 많았다.

- chat repository DTO
- concrete Postgres adapter
- shared Redis concrete factory
- domain-aware UoW accessor

그래서 public은 "generic execution seam"만 노출하고, 내부 구현은 `storage/*`에 남겼다.

이 구조가 필요한 이유는:

- core가 도메인 schema에 물들지 않게 하기 위해
- 서버와 tool이 같은 execution substrate를 쓰게 하기 위해
- package-first consumer가 최소 인터페이스만 보게 하기 위해

### 8.2 왜 `DbWorkerPool`이 transaction을 worker 내부에서 여는가

`DbWorkerPool`은 job이 들어올 때 외부에서 UoW를 넘겨받지 않는다. worker 내부에서 pool로부터 `make_unit_of_work()`를 호출한다.

이렇게 한 이유:

- worker thread 경계와 DB connection lifetime을 일치시키기 위해
- failure를 job 단위로 rollback시키기 위해
- submitter가 connection lifetime을 잘못 들고 다니지 못하게 하기 위해

즉, storage worker는 단순 thread-pool이 아니라 "transaction boundary executor"다.

### 8.3 왜 retry/backoff가 helper로 올라왔는가

재시도 정책을 각 루프에 묻어두면 다음 문제가 생긴다.

- 지연 계산이 경로마다 달라짐
- jitter 적용이 누락됨
- 운영 문서와 실제 코드가 어긋남

그래서 `RetryBackoffPolicy`가 core public surface가 되었다. 이것은 단순 유틸이 아니라 "retry 의미를 공유된 계약으로 만드는 장치"다.

## 9. 공유 상태/발견 아키텍처: `discovery` vs `state`

### 9.1 왜 stable discovery는 selector/backend interface까지만 올랐는가

공개된 discovery surface는 다음까지만 책임진다.

- `InstanceRecord`
- `InstanceSelector`
- selector classification helpers
- `IInstanceStateBackend`
- `InMemoryStateBackend`
- lifecycle policy serialization

이게 핵심인 이유는 이것이 "공용 개념"이기 때문이다.

- instance inventory
- filtering
- generic backend seam

반대로 stable로 올리지 않은 것:

- Redis adapter construction
- Consul adapter
- gateway sticky `SessionDirectory`

이들은 공용 개념이 아니라 배치/앱 전략이다.

### 9.2 왜 `discovery/*.hpp`가 `state/*.hpp` wrapper인가

이 역시 facade 전략이다.

- canonical public namespace는 `server::core::discovery`
- 구현 소유권과 transitional seam은 `server::core::state`

이렇게 하면 consumer는 더 좋은 public 이름을 쓰고, 내부는 단계적으로 정리할 수 있다.

### 9.3 Redis registry adapter가 internal인 이유

`RedisInstanceStateBackend`는 공유 구현이지만 public으로 올리기엔 아직 coupling이 남아 있다.

- Redis key schema
- TTL semantics
- reload cache cadence
- concrete Redis client contract shape

이것들은 분명 reusable해 보이지만, 한번 stable로 올리면 provider/adapter 전략을 고정하게 된다. 따라서 지금은 core-owned internal seam으로 남겨 두는 것이 맞다.

## 10. 확장성 아키텍처: plugin, scripting

이 영역은 "mechanism은 stable, domain ABI는 app-owned"라는 철학이 가장 분명하게 드러난다.

### 10.1 왜 mechanism과 ABI를 분리했는가

예전에는 plugin/Lua 기능을 통째로 chat feature처럼 보기 쉬웠다. 하지만 실제로 reusable한 것은 두 층으로 갈린다.

- 재사용 가능한 mechanism
  - shared library loading
  - hot reload
  - directory scan
  - file watching
  - Lua sandbox/runtime
- service-specific contract
  - chat hook ABI
  - chat Lua bindings
  - chat conflict policy

stable로 올린 것은 전자다. 후자는 여전히 앱 계층이다.

이 분리가 필요한 이유:

- mechanism은 다른 consumer도 재사용 가능
- ABI까지 stable로 올리면 특정 도메인 semantics를 core가 떠안게 됨

### 10.2 `PluginHost`가 왜 cache-copy를 하는가

직접 원본 바이너리를 열면 배포 중 overwrite, partial copy, file lock 문제가 생긴다. 그래서 `PluginHost`는 cache copy 후 load를 기본으로 한다.

이 구조의 장점:

- 배포 중 반쯤 복사된 파일을 직접 읽지 않음
- 원본 파일 lock 경합을 줄임
- load 실패 시 현재 loaded plugin은 유지 가능

즉, hot reload에서 제일 중요한 "안전한 교체"를 구현하는 장치다.

### 10.3 lock/sentinel path가 왜 있는가

플러그인이나 스크립트는 배포 중간 상태를 볼 가능성이 높다. `lock_path` 또는 sentinel을 두는 이유는 "변경 감지보다 안전한 무시"를 선택하기 위해서다.

핫 리로드에서는 놓치는 한 번보다, 반쯤 배포된 artifact를 읽는 것이 훨씬 위험하다.

### 10.4 `PluginChainHost`가 왜 정렬된 immutable chain을 노출하는가

체인 순서는 semantics다. 따라서:

- directory scan 결과는 filename 순서로 고정
- 현재 chain은 atomic shared pointer로 교체
- reader는 lock-free에 가까운 읽기

즉, reload path에는 mutex가 있어도 execution path는 가능한 한 안정적으로 유지하려는 구조다.

### 10.5 `ScriptWatcher`가 왜 polling 기반인가

OS file notification API는 플랫폼별 편차가 크고, 컨테이너/네트워크 파일시스템에서는 의미가 달라질 수 있다. 현재 프로젝트는 portability와 예측 가능성을 위해 `last_write_time` polling을 선택했다.

장점:

- 플랫폼/배포 환경 차이가 적다
- lock file 정책과 합치기 쉽다
- order와 diff semantics를 직접 제어할 수 있다

단점:

- change detection latency는 poll 주기에 의존한다

이 프로젝트는 핫 리로드에서 즉시성보다 안전성과 단순성을 선택했다.

### 10.6 `LuaRuntime`이 "스크립트 VM 보존" 대신 "호출 시 새 상태"에 가까운 구조인 이유

현재 구현은 스크립트 로드 시 syntax/limit validation을 하고, 실제 call 시 sandboxed state를 구성해 실행하는 쪽에 가깝다.

이 선택의 장점:

- script 간 state leak이 적다
- host API binding을 deterministic하게 다시 구성할 수 있다
- instruction/memory limit enforcement가 단순하다
- reload가 "loaded path 교체"만으로 명확해진다

즉, 최대 성능보다 "안전하고 재현 가능한 스크립트 실행"을 택한 것이다.

### 10.7 왜 Lua sandbox는 허용 목록 중심인가

기본 정책은 다음만 허용한다.

- `base`
- `string`
- `table`
- `math`
- `utf8`

그리고 다음은 막는다.

- `os`
- `io`
- `debug`
- `package`
- `ffi`
- `dofile`
- `loadfile`
- `require`

이건 보안뿐 아니라 determinism 때문이다. 런타임 hook에서 파일 시스템, 프로세스, 동적 로딩, FFI가 열리면 운영 예측성이 급격히 떨어진다.

## 11. realtime 아키텍처: engine capability를 public contract로 분리

### 11.1 왜 `realtime/**`는 transport implementation이 아니라 capability contract인가

이 표면은 gateway UDP 엔진이나 game-specific schema를 public으로 노출하지 않는다. 대신 다음을 공개한다.

- fixed-step tick accumulation
- input staging semantics
- snapshot/delta shaping
- rewind/history lookup
- direct bind payload contract
- direct attach/delivery policy
- sequenced UDP quality tracking

즉, core는 "실시간 게임 플레이 substrate의 엔진 중립적 부분"만 뽑아서 공개한다.

### 11.2 왜 `WorldRuntime`이 앱 wire format을 모르는가

`WorldRuntime`의 출력은 `ReplicationUpdate`다. protobuf나 opcode를 직접 만들지 않는다.

이 구조가 필요한 이유:

- runtime algorithm과 wire encoding을 분리하려고
- 같은 authoritative state 모델을 여러 앱/전송 경로가 재사용할 수 있게 하려고
- transport canary/fallback과 gameplay state를 독립적으로 진화시키려고

### 11.3 왜 direct transport 정책이 별도 header로 쪼개져 있는가

직접 전송 경로는 사실 여러 문제가 섞여 있다.

- bind ticket payload
- rollout / canary selection
- per-message direct delivery decision
- packet quality accounting

이를 한 객체에 몰아넣으면 transport layer와 gameplay policy가 강하게 엉킨다. 그래서 core는 그것들을 작은 정책 계약으로 쪼갰다.

- `direct_bind.hpp`
- `transport_policy.hpp`
- `direct_delivery.hpp`
- `transport_quality.hpp`

이 설계는 매우 중요하다. 공개 contract는 policy와 payload shape만 고정하고, gateway의 실제 UDP/RUDP machinery는 internal로 남길 수 있기 때문이다.

## 12. world orchestration 아키텍처: provider-neutral control-plane vocabulary

`worlds/**`는 이 저장소의 가장 독특한 core surface다.

### 12.1 왜 이 계층은 대부분 header-only data/evaluation contract인가

이 계층은 구체적인 controller가 아니다. 목적은 "어떤 상태를 관측했고, 그 상태를 어떻게 해석해야 하는가"를 정규화하는 것이다.

그래서 제공하는 것은 mostly:

- document structs
- enums
- summary/status structs
- pure evaluation helpers

예:

- desired vs observed topology reconciliation
- actuation plan / request / execution / realization / adapter status
- world drain / transfer / migration status
- Kubernetes-first orchestration interpretation
- AWS-first provider interpretation

이렇게 해야 다음이 가능하다.

- admin/control-plane이 같은 vocabulary를 사용
- provider adapter가 core contract 위에서 동작
- core가 Kubernetes SDK, AWS SDK, Docker API에 묶이지 않음

### 12.2 왜 topology 계층이 이렇게 세분화되었는가

`topology.hpp`는 한눈에 보면 과하다. 하지만 각 층은 섞으면 안 되는 의미를 분리한다.

- desired document
- observed pools
- reconciliation
- read-only actuation plan
- operator-approved request
- executor progress
- realization status
- adapter lease
- runtime assignment

이걸 하나의 "topology status"로 뭉치면 무엇이 operator intent인지, 무엇이 execution progress인지, 무엇이 observation인지 구분할 수 없게 된다.

즉, 세분화는 복잡성을 늘린 것이 아니라 책임을 분리한 것이다.

### 12.3 왜 `kubernetes.hpp`와 `aws.hpp`가 모두 존재하는가

이 둘의 목적은 provider SDK 통합이 아니다. 오히려 generic topology/world contract를 provider vocabulary로 번역하는 것이다.

- Kubernetes:
  - workload replicas
  - ready pods
  - runtime assignment publish
  - drain-to-retire orchestration
- AWS:
  - load balancer attachment
  - target health
  - managed Redis/Postgres naming
  - runtime assignment satisfaction

즉, provider-specific implementation을 core에 넣는 것이 아니라, provider별 "해석 contract"만 넣는다.

이 설계 덕분에 core는:

- control-plane 문서 형식을 공용으로 유지하면서
- Kubernetes-first, AWS-first adapter를 각각 설명할 수 있다

## 13. leaf utility 아키텍처: 작지만 의도적인 공용 조각들

### 13.1 `compression`

LZ4를 쓴 이유는 이 프로젝트의 실시간 성격 때문이다.

- 압축률보다 CPU/latency가 더 중요
- 대역폭 절감은 필요하지만 gzip류의 무거운 압축은 부담

즉, "최고 압축률"이 아니라 "실시간 경로에서 감당 가능한 압축"을 택했다.

### 13.2 `security::Cipher`

AES-256-GCM을 택한 이유는 기밀성뿐 아니라 인증 태그 검증이 필요하기 때문이다. 실시간/네트워크 경로에서 "복호화는 되지만 변조되었을 수 있음" 같은 애매한 상태를 허용하지 않으려는 설계다.

### 13.3 `build_info`

build info는 generated source가 아니라 compile definition으로 주입된다. 이유는 단순하다.

- generated file 관리 부담을 줄이고
- source archive에서도 fallback 가능하게 하고
- 모든 바이너리가 값 접근을 매우 싸게 하도록 만들기 위해서다

### 13.4 `api/version.hpp`

public API version을 헤더에 명시하는 것은 package-first 설계의 일부다. stable surface 변경이 단순 git diff가 아니라 consumer-facing semantic version 변화라는 점을 코드 레벨에서 드러낸다.

## 14. core가 의도적으로 소유하지 않는 것들

이 문서에서 가장 중요한 부분 중 하나다. core의 강점은 무엇을 넣었는가만큼, 무엇을 넣지 않았는가에서 나온다.

core 밖에 남기는 이유가 명확한 것들:

- 채팅 서비스 규칙과 room/message/user repository
- gateway sticky `SessionDirectory`
- concrete Redis/Consul/Postgres adapter 전략
- chat plugin ABI 및 chat Lua binding
- app bootstrap의 listener/route/worker 상세 wiring
- provider SDK 직접 호출

왜냐하면 이것들은 reusable substrate가 아니라 app/integration concern이기 때문이다.

## 15. 현재 설계의 trade-off

이 구조가 완벽하다는 뜻은 아니다. 비용도 분명히 있다.

### 15.1 global + local registry 이중 구조는 복잡하다

`EngineContext`와 `ServiceRegistry`의 병존은 이상적 구조가 아니라 과도기 구조다. 하지만 현재는 필요하다.

### 15.2 `Session`과 `Connection`의 공존은 개념 중복처럼 보일 수 있다

맞다. 하지만 둘을 섣불리 합치면 transport/public boundary가 깨진다.

### 15.3 polling watcher는 event-driven watcher보다 반응이 느리다

그렇지만 배포 안정성과 portability를 위해 감수한 비용이다.

### 15.4 `worlds/topology.hpp`는 매우 크다

맞다. 대신 control-plane의 상태 vocabulary를 한 파일에서 일관되게 볼 수 있다. 이 계층은 구현 복잡성을 숨기는 대신 "상태 해석 계약을 분산시키지 않는 것"을 택했다.

### 15.5 `runtime_metrics`는 instance-scoped가 아니다

그 대신 hot path 비용이 낮고, `EngineRuntime::snapshot()`과 역할 분담이 명확하다.

## 16. 앞으로도 유지해야 할 설계 규칙

### 16.1 core는 앱을 보면 안 된다

새 공용 helper가 필요해 보여도 `server/` 구현 타입을 include하는 순간 core boundary가 무너진다.

### 16.2 public으로 올릴 것은 "구현"이 아니라 "계약"이어야 한다

특히 storage/discovery/provider 영역에서 concrete adapter를 성급히 stable로 올리면 future refactor 비용이 폭증한다.

### 16.3 lifecycle와 observability는 feature 부가물이 아니라 core의 일부다

shutdown, readiness, metrics, logs를 앱이 각자 다시 구현하게 두면 지금까지 만든 공통 runtime 의미가 사라진다.

### 16.4 overload behavior는 반드시 bounded semantics를 가져야 한다

queue, retry, inflight, memory pool, HTTP hardening 모두 같은 메시지를 가진다.

- 무제한 증가는 피한다
- 실패는 명시적으로 드러낸다
- 관측 가능해야 한다

### 16.5 provider/world contract는 data-first를 유지해야 한다

SDK 호출이나 controller 구현을 core에 넣는 순간, world surface는 reusable engine contract가 아니라 특정 배포물의 implementation detail로 변한다.

## 17. 결론

현재 `server_core`의 핵심 설계는 한 줄로 요약할 수 있다.

공용 실행 규약은 core가 소유하되, 앱 의미와 배포 구현은 core 밖에 남긴다.

이 문장이 구체적으로 풀린 결과가 지금의 구조다.

- `core/app`: 공통 lifecycle과 composition
- `core/net`: generic transport substrate
- `Session`: current packet session implementation이므로 internal
- `core/concurrent`, `core/memory`: bounded execution과 predictable resource use
- `core/metrics`, `runtime_metrics`, `util/log`, `trace`: 운영성을 first-class로 취급
- `core/storage_execution`, `core/discovery`: 좁은 public seam + 내부 구현 분리
- `core/plugin`, `core/scripting`: mechanism stable, domain ABI app-owned
- `core/realtime`, `core/worlds`: implementation이 아니라 capability/data contract를 public으로 제공

이 구조는 단순히 "예쁘게 나눈 것"이 아니라, 다음을 동시에 만족시키기 위해 필요했다.

- 여러 바이너리의 공통 운영 규약
- package-first public API
- domain neutrality
- overload safety
- provider neutrality
- incremental evolution without ABI collapse

따라서 core를 바꿀 때 가장 먼저 던져야 할 질문은 기능 질문이 아니라 경계 질문이다.

- 이것이 정말 모든 consumer에 공통인 규약인가?
- 아니면 현재 앱/배포 구현이 우연히 공유하고 있는 것뿐인가?

현재 아키텍처는 그 질문에 대한 축적된 답변이다.
