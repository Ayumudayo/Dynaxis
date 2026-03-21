# 확장성 거버넌스

이 문서는 plugin/Lua 확장성을 `server_core`의 공용 플랫폼 능력(capability)으로 운영하기 위한 현재 거버넌스 규칙을 정리한다. 현재 상태와 문서 진입점은 `docs/extensibility/overview.md`를 기준으로 본다.

이 문서가 필요한 이유는, 확장성 기능은 "돌아가면 되는 부가 기능"이 아니기 때문이다. 외부 artifact를 런타임에 불러와 실행하는 순간, ABI 안정성, 재로드, 샌드박스, 장애 복구, 배포 충돌 같은 문제가 함께 생긴다. 이때 무엇이 core 계약이고, 무엇이 service 정책인지 경계가 없으면 작은 변경도 쉽게 breaking change가 된다. 그래서 거버넌스 문서는 구현보다 먼저 "어디까지를 약속으로 볼 것인가"를 정한다.

핵심 분류는 다음과 같다.

- `core/include/server/core/plugin/*`, `core/include/server/core/scripting/*`
  - 서비스 재사용을 위한 메커니즘 계층
- `server/include/server/chat/chat_hook_plugin_abi.hpp`, `server/src/chat/*`, `server/src/scripting/chat_lua_bindings.cpp`
  - 첫 번째 구체 소비자인 chat service 계층
- 현재 단계의 해석
  - 확장성은 app 편의 기능이 아니라 "stable core mechanism + transitional service contract"다.

## 1. 능력 경계(capability boundary)

### 1.1 core가 소유하는 메커니즘 계층

다음 표면은 여러 서비스가 재사용할 수 있어야 하므로 core가 소유한다.

- `shared_library.hpp`
  - 동적 라이브러리 load/unload를 RAII로 관리한다.
- `plugin_host.hpp`
  - 단일 플러그인의 load/reload/cache-copy/validator를 담당한다.
- `plugin_chain_host.hpp`
  - 다중 플러그인 체인 조립과 디렉터리 스캔을 담당한다.
- `script_watcher.hpp`
  - 스크립트/파일 변경 감시와 lock/sentinel 의미를 담당한다.
- `lua_sandbox.hpp`
  - Lua 허용 라이브러리와 자원 제한 정책을 담당한다.
- `lua_runtime.hpp`
  - 스크립트 load/call/reload, host API 등록, 메트릭 수집을 담당한다.

이 계층을 core에 두는 이유는, 이런 문제들이 특정 서비스의 도메인 정책이 아니라 "외부 artifact를 안전하게 실행하는 공통 기술 문제"이기 때문이다. 만약 각 서비스가 같은 기능을 따로 구현하면, loader precedence와 watcher semantics가 조금씩 달라져 운영이 혼란스러워지고, 설치 패키지나 외부 소비자 검증도 서비스마다 다시 해야 한다.

### 1.2 서비스가 소유하는 정책 계층

다음 표면은 chat 도메인 의미와 직접 연결되므로 서비스가 소유한다.

- chat plugin ABI 버전과 hook payload 형태
- chat plugin chain의 decision 해석과 metrics naming
- chat Lua host binding과 허용 action
- control-plane rollout 정책과 hook conflict 정책

이 계층을 core로 끌어올리지 않는 이유도 분명하다. 도메인 계약까지 core가 소유하기 시작하면, 이후 다른 서비스가 전혀 다른 event 모델과 payload를 필요로 할 때 core가 chat 중심 설계에 끌려가게 된다. core는 메커니즘만 안정화하고, 도메인 의미는 서비스가 책임지는 편이 장기적으로 훨씬 단순하다.

## 2. 호환성 수준

### 2.1 Stable

다음 메커니즘 헤더는 외부 소비자 관점의 검증과 설치 패키지 증명(proof)을 확보했으므로 일반적인 `Stable` API 규칙을 적용한다.

- `core/include/server/core/plugin/shared_library.hpp`
- `core/include/server/core/plugin/plugin_host.hpp`
- `core/include/server/core/plugin/plugin_chain_host.hpp`
- `core/include/server/core/scripting/script_watcher.hpp`
- `core/include/server/core/scripting/lua_sandbox.hpp`
- `core/include/server/core/scripting/lua_runtime.hpp`

`Stable`로 분류한다는 것은, 여기서 생기는 변경이 단순 내부 정리가 아니라 소비자 계약 변경으로 읽혀야 한다는 뜻이다. 이 분류가 없으면 설치 패키지를 쓰는 외부 코드나 다른 서비스가 예고 없이 깨질 수 있다.

### 2.2 Transitional

다음은 아직 빠른 진화가 허용되는 서비스 계약 계층이다.

- chat hook ABI
- chat plugin chain 정책
- chat Lua binding

여기서는 API/ABI 진화가 가능하지만 아무렇게나 바꿀 수는 없다. 다음 원칙을 지켜야 한다.

- 변경은 문서화되어야 한다.
- 변경은 테스트로 증명되어야 한다.
- additive인지 breaking인지 구분되어야 한다.
- loader precedence, reload semantics, host-call 의미, hook decision 의미가 바뀌면 같은 변경에서 관련 문서도 함께 갱신해야 한다.

즉 `Transitional`은 "마음대로 바꿔도 된다"는 뜻이 아니라, 아직 안정화 이전 단계이므로 변경을 허용하되 그 비용을 문서와 테스트로 지불해야 한다는 뜻이다.

### 2.3 Internal

다음은 외부 계약으로 소비되면 안 되는 내부 표면이다.

- service-private helper
- test-only seam
- 구현 세부 사항

이 구분이 필요한 이유는, 내부 helper가 우연히 외부에서 사용되기 시작하면 이후 작은 리팩터링도 breaking change 취급을 받게 되기 때문이다. internal을 internal로 남겨 두는 것이 결국 core surface를 작고 튼튼하게 유지하는 방법이다.

## 3. 플러그인 ABI 거버넌스

### 3.1 버전 규칙

- ABI 버전은 명시적 숫자 상수와 명시적 entrypoint로 드러나야 한다.
  - 예: `chat_hook_api_v2()`
- breaking ABI 변경은 기존 struct나 enum을 조용히 바꾸는 방식이 아니라, 새 버전 상수와 새 entrypoint symbol로 표현해야 한다.
- 기존 ABI entrypoint는 후속 버전이 도입된 뒤 최소 한 릴리스 주기 동안은 유지하는 편이 원칙이다.

이 규칙을 지키지 않으면 가장 위험한 형태의 회귀가 생긴다. 컴파일은 되지만 런타임에서 조용히 오동작하는 식의 binary compatibility 문제다. ABI는 문법보다 배포 후 영향이 더 크므로, "기존 것을 몰래 바꾸지 않는다"는 원칙이 중요하다.

### 3.2 breaking change로 보는 것

다음 변경은 breaking으로 본다.

- export entrypoint 제거 또는 이름 변경
- enum 의미 변경 또는 기존 enum 값 재사용
- 기존 struct field 제거 또는 재정렬
- 기존 ABI에 대해 validator 요구사항을 version bump 없이 강화
- deny/modify/handled 의미를 기존 플러그인이 깨질 정도로 바꾸는 변경

이 기준이 필요한 이유는, 플러그인은 보통 별도 artifact로 배포되므로 "소스가 같이 바뀌면 괜찮다"는 가정을 할 수 없기 때문이다. ABI 변경을 느슨하게 관리하면 운영 중 롤링 배포에서 구버전과 신버전이 섞이는 순간 문제가 터질 수 있다.

### 3.3 additive change로 보는 것

전이 단계에서 다음 작업은 additive로 허용할 수 있다.

- 문서 명확화
- 기존 의미를 보존하는 신규 테스트 또는 메트릭 추가
- 현재 loader/validator 의미를 바꾸지 않는 선택적(optional) 동작 추가

반대로 binary layout이나 필수 동작을 바꾸면 additive처럼 보이더라도 새 ABI 버전으로 다뤄야 한다. additive 판정을 느슨하게 하면 결국 breaking change를 숨기는 결과가 된다.

## 4. Lua 런타임 거버넌스

### 4.1 능력 정책

- 공식 빌드는 Lua capability를 포함한다.
- 실제 런타임 활성화는 `LUA_ENABLED`와 스크립트 디렉터리 설정으로 제어한다.
- `enabled()`는 바이너리에 기능이 들어 있는지 여부가 아니라, 현재 런타임에서 실제로 활성 상태인지 반영한다.

이 구분이 필요한 이유는 "빌드에 포함됐다"와 "운영에서 켰다"를 섞으면 운영 문서와 실제 동작이 엇갈리기 쉽기 때문이다. capability 포함 여부는 배포 artifact의 성질이고, `LUA_ENABLED`는 운영 토글이다. 둘을 분리해야 rollout과 emergency disable이 명확해진다.

### 4.2 스크립트 정책 보장

Lua는 유연하지만 그만큼 경계가 흐려지기 쉽다. 현재 거버넌스는 다음 항목을 명시적 보장으로 본다.

- 허용 라이브러리는 문서화되고 의도적으로 제한되어야 한다.
- instruction/memory limit은 구현 우연이 아니라 운영 계약의 일부다.
- auto-disable과 reload recovery는 문서와 테스트가 함께 유지되어야 한다.
- 함수형 hook(`function on_<hook>(ctx)`)이 기본 작성 모델이다.
- directive/return-table은 fallback/testing aid로만 남긴다.

이 보장이 필요한 이유는, Lua는 조금만 경계가 느슨해져도 성능 문제와 보안 문제가 동시에 생기기 쉽기 때문이다. 샌드박스와 자원 제한을 "옵션"처럼 취급하면 운영에서 예측 가능한 동작을 보장할 수 없다.

### 4.3 Lua에서 breaking change로 보는 것

전이 단계라 하더라도 다음 변경은 breaking으로 취급한다.

- 문서화된 샘플이 사용하는 host API table/function 제거
- 널리 쓰는 `ctx` field 이름 변경 또는 삭제
- 기존 문서화된 스크립트를 깨는 샌드박스 기본값 변경
- hook decision 의미나 host-call error handling을 비호환적으로 바꾸는 변경

Lua는 "스크립트니까 빨리 바꾸면 된다"는 착각이 생기기 쉽지만, 실제 운영에서는 스크립트도 배포 artifact이므로 계약으로 다뤄야 한다. 문서에 나온 샘플이 갑자기 깨지기 시작하면 현장 운영자에게는 곧바로 장애가 된다.

## 5. 단계별 안정화 계획

### 5.1 1단계 - 메커니즘 강화 (완료)

- `plugin_host`, `plugin_chain_host`, `script_watcher`, `lua_sandbox`, `lua_runtime`는 core 테스트와 stable public API/package proof를 확보했다.
- 서비스 특화 ABI와 binding 로직은 core 밖에 둔다.

이 단계가 먼저 필요한 이유는, 메커니즘이 흔들리는 상태에서 서비스 계약을 고정해 봐야 의미가 없기 때문이다. 공통 로더와 런타임을 먼저 단단하게 만들어야 그 위의 서비스 계약도 안정적으로 관리할 수 있다.

### 5.2 2단계 - 서비스 계약 강화

- chat hook ABI와 chat Lua binding을 서비스 소유 계약으로 계속 문서화한다.
- deny/modify propagation, auto-disable, ordering, sample artifact를 server-level test로 강제한다.
- control-plane rollout/conflict 문서를 실제 동작과 맞춘다.

이 단계의 목적은 "빠르게 바뀔 수 있는 부분"을 방치하지 않고, 서비스 수준에서 명시적으로 붙잡는 것이다. core가 안정적이어도 서비스 계약이 자주 흔들리면 운영자는 여전히 예측하기 어렵다.

### 5.3 3단계 - 외부 소비자 증명 (core 메커니즘 기준 완료)

- `CorePublicApiExtensibilitySmoke`
  - stable header 기준의 plugin load/chain, Lua runtime/watcher를 검증한다.
- `CoreInstalledPackageConsumer`
  - 설치 패키지 경로에서도 동일한 메커니즘이 동작하는지 검증한다.
- 결과
  - 재사용 가능한 core 메커니즘 헤더는 `Stable`
  - chat 전용 계약은 `Transitional`

외부 소비자 증명이 중요한 이유는, "우리 저장소 안에서는 된다"와 "설치된 패키지에서도 계약이 유지된다"는 전혀 다른 수준의 보장이라서다.

## 6. 마이그레이션 노트 정책

- breaking extensibility 변경은 `docs/core-api/migration-note-template.md`를 사용해 마이그레이션 노트를 남겨야 한다.
- 같은 변경에서 아래도 함께 갱신해야 한다.
  - `docs/core-api/extensions.md`
  - 영향받는 quickstart/운영 문서
  - old/new behavior를 검증하는 관련 테스트
- PR에는 변경 성격을 `additive`, `behavioral`, `breaking` 중 하나로 명시해야 한다.

이 절차가 번거로워 보여도 필요한 이유는, 확장성 변경은 보통 운영 artifact와 바로 연결되기 때문이다. migration note가 없으면 운영자는 "업데이트했더니 왜 기존 스크립트/플러그인이 안 붙는지"를 diff를 뒤져서 스스로 알아내야 한다.

## 7. 확장성 변경에 필요한 증거

확장성 변경을 완료로 보려면 최소한 다음 증거가 있어야 한다.

- 변경된 문서가 같은 변경 안에서 함께 갱신되었다.
- 관련 core/server 테스트가 통과했다.
- loader/runtime semantics가 바뀌었다면 rollback 또는 fallback 동작을 시연했다.
- compatibility claim이 바뀌었다면 matrix/boundary 문서도 함께 갱신했다.

핵심은 단순하다. extensibility는 코드만 맞다고 끝나는 기능이 아니다. 문서, 테스트, 운영 복구 경로까지 함께 맞아야 실제로 유지보수 가능한 기능이 된다.
