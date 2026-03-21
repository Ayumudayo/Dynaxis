# 확장성 개요

Dynaxis는 현재 네이티브 플러그인과 Lua 기반 확장성을 "채팅 기능에 임시로 덧붙인 부가 기능"이 아니라, 여러 서비스가 재사용할 수 있는 공용 플랫폼 능력(capability)으로 다룬다. 이 문서는 그 큰 그림을 먼저 설명한다.

이 구분이 중요한 이유는 단순하다. 확장 기능을 특정 앱의 편의 기능처럼 취급하면, 처음에는 빠르게 붙일 수 있어도 시간이 지나면서 로더, 재로드(reload), 샌드박스, 장애 복구 같은 공통 문제가 서비스별로 중복된다. 그러면 운영 중 문제가 생겼을 때 "메커니즘이 깨진 것인지, chat 계약이 깨진 것인지"를 분리해 판단하기 어려워진다. 반대로 공용 메커니즘과 서비스별 계약을 나누면 재사용과 장애 분석, 테스트 책임이 훨씬 선명해진다.

현재 Dynaxis는 이 구조를 다음처럼 본다.

- `server_core`는 재사용 가능한 메커니즘 계층을 제공한다.
- `server_app`은 그 메커니즘을 실제 chat 도메인에 연결하는 첫 번째 소비자(consumer)다.
- 그래서 core 메커니즘은 `Stable`에 가깝게 다루고, chat ABI와 바인딩은 아직 `Transitional`로 관리한다.

## 현재 제공 능력

지금 저장소가 실제로 제공하는 확장성 능력은 크게 두 축이다.

- 네이티브 플러그인
  - `server_core`의 plugin host가 동적 라이브러리 로드, 검증, 재로드, 체인 구성을 담당한다.
- Lua 스크립트
  - `server_core`의 scripting 계층이 스크립트 로드, 샌드박스, 재로드, 메트릭 수집을 담당한다.

이 두 기능을 core에 둔 이유는, 서비스마다 다시 같은 로더와 watcher를 구현하게 두면 유지보수 비용이 빠르게 커지기 때문이다. 로더 precedence, 파일 감시, auto-disable, fallback 정책은 채팅만의 문제가 아니라 "런타임에서 외부 artifact를 안전하게 다루는 법"이라는 공용 문제다. 이런 문제를 앱마다 따로 풀면 정책도 조금씩 달라지고, 문서와 테스트도 쉽게 어긋난다.

반면 `server_app`은 chat에 특화된 부분만 소유한다.

- chat hook ABI
- hook payload 의미
- chat 전용 Lua host binding
- admin/control-plane rollout 경로

이 분리가 필요한 이유는 service contract까지 core로 올리면, 나중에 다른 서비스가 전혀 다른 hook 의미나 payload를 필요로 할 때 core를 불필요하게 흔들게 되기 때문이다. 공용 메커니즘은 넓게 재사용할수록 좋아지지만, 도메인 의미까지 섞이면 오히려 core가 커지고 불안정해진다.

## 소유권 경계

현재 기준으로 ownership boundary는 다음처럼 읽는 것이 가장 안전하다.

- `core/include/server/core/plugin/*`
  - 동적 로드, 재로드, 체인 조립, 검증 같은 공용 메커니즘
- `core/include/server/core/scripting/*`
  - Lua 런타임, 샌드박스, watcher, 메트릭 같은 공용 메커니즘
- `server/include/server/chat/chat_hook_plugin_abi.hpp`
  - chat 전용 ABI 계약
- `server/src/chat/*`
  - chat hook 정책, 적용 순서, 결과 해석
- `server/src/scripting/chat_lua_bindings.cpp`
  - chat가 Lua에 노출하는 host API

이 경계를 의도적으로 유지하지 않으면 두 가지 문제가 생긴다.

- 메커니즘 변경과 도메인 정책 변경이 한 덩어리 diff로 섞여 리뷰가 어려워진다.
- 테스트 실패가 났을 때 "core regression"인지 "chat contract regression"인지 분리하기 어려워진다.

그래서 이 문서의 핵심 메시지는 단순하다. 확장성은 하나의 기능이 아니라 "공용 메커니즘 + 서비스별 계약"의 조합이며, 이 둘을 분리해 보는 것이 장기 유지보수에 유리하다.

## 운영 관점에서 기대해야 할 것

확장성 기능을 운영에 올릴 때는 단순히 "로드가 되느냐"만 보면 안 된다. 실제로는 다음 항목이 함께 계약으로 읽혀야 한다.

- 재로드 동작
  - 파일 교체 후 언제 반영되는지
- 샌드박스 제한
  - 어떤 라이브러리와 자원 사용이 허용되는지
- 자동 비활성화(auto-disable)
  - 연속 오류가 날 때 시스템이 어떻게 안전하게 후퇴하는지
- fallback
  - primary artifact가 비었거나 깨졌을 때 대체 경로가 있는지
- rollout / conflict semantics
  - 같은 hook 영역에 여러 artifact를 어떻게 배치하고 차단하는지

이 항목들을 구현 세부로 치부하면 운영 문서와 런타임 동작이 쉽게 어긋난다. 반대로 이를 명시적 계약으로 문서화하면, 장애 시 "버그인지 설계된 안전장치인지"를 훨씬 빨리 판단할 수 있다.

## 작성과 실행의 기본 원칙

현재 Dynaxis는 다음 원칙을 기준으로 확장성을 노출한다.

- hot path는 네이티브 우선이다.
  - 지연에 민감한 경로는 네이티브 플러그인이 먼저 담당한다.
- Lua는 cold-hook 정책/커스터마이징에 적합하다.
  - 빠른 수정, 운영 실험, 정책 분리에 강하다.
- Lua 작성의 기본 모델은 `function on_<hook>(ctx)`다.
  - directive/return-table은 호환성 유지와 테스트 보조 용도로만 둔다.

이 기준이 필요한 이유는, 모든 경로를 같은 방식으로 확장하려 하면 성능 요구가 다른 경로까지 같은 제약으로 다루게 되기 때문이다. 네이티브와 Lua를 역할에 맞게 나누면, 운영 유연성과 성능 예산을 동시에 맞추기 쉬워진다.

## 검증 경로

확장성 관련 변경은 문서만 바꾸고 끝내면 안 된다. 현재 저장소는 최소한 아래 검증 경로를 기준으로 읽는 편이 맞다.

- `tests/server/`
  - chat hook 통합 동작, auto-disable, 적용 순서 검증
- `tests/core/`
  - plugin host, chain host, Lua runtime, Lua sandbox 검증
- `CorePublicApiExtensibilitySmoke`
  - stable header 기준 core 메커니즘 smoke 검증
- `CoreInstalledPackageConsumer`
  - 설치 패키지 경로에서도 메커니즘이 동일하게 동작하는지 검증
- `tests/python/verify_admin_api.py`
- `tests/python/verify_admin_auth.py`
- `tests/python/verify_admin_read_only.py`
  - 제어면(control plane) rollout과 권한 경계 검증

이 검증이 중요한 이유는, extensibility는 코드 한 조각의 문제가 아니라 로더, 샌드박스, 운영 API, 배포 정책이 함께 맞아야 성립하는 기능이기 때문이다.

## 다음에 읽을 문서

아래 문서는 서로 역할이 다르다. 순서대로 읽으면 전체 구조를 이해하기 쉽다.

- `docs/extensibility/governance.md`
  - 무엇이 core 책임이고 무엇이 service 책임인지, 어떤 변경이 breaking인지 설명한다.
- `docs/extensibility/control-plane-api.md`
  - 운영자가 artifact를 조회, precheck, 배포, 예약하는 API 계약을 설명한다.
- `docs/extensibility/conflict-policy.md`
  - 같은 hook 영역에 여러 artifact가 있을 때 충돌을 어떻게 막는지 설명한다.
- `docs/extensibility/plugin-quickstart.md`
  - 네이티브 플러그인을 빠르게 작성, 배포, 롤백하는 절차를 설명한다.
- `docs/extensibility/lua-quickstart.md`
  - Lua cold hook을 빠르게 작성, 검증, 롤백하는 절차를 설명한다.
- `docs/extensibility/recipes.md`
  - 자주 쓰는 운영 패턴을 예시 중심으로 정리한다.
- `docs/core-api/extensions.md`
  - core public API 기준에서 확장성 표면을 어떻게 약속하는지 설명한다.
