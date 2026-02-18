# P2 Network Abstraction Boundary Plan

## 1. Goal

`Acceptor`/`Listener`, `Session`/`Connection` naming overlap의 영향 반경을 고정하고,
행동 통합 대상/비대상을 먼저 확정한 뒤 안전한 단계적 rename/integration 순서를 정의한다.

## 2. Current Boundary (As-Is)

- `server_app` ingress: `core::net::SessionListener` + `core::net::Session` (legacy base: `core::Acceptor` + `core::Session`)
  - construction: `server/src/app/bootstrap.cpp`
- `gateway_app` ingress: `core::net::TransportListener` + `core::net::TransportConnection` (`GatewayConnection` 파생)
  - construction: `gateway/src/gateway_app.cpp`
  - inheritance: `gateway/include/gateway/gateway_connection.hpp`

핵심 관찰:

- `Acceptor`와 `Listener`는 둘 다 accept loop를 담당하지만, 현재 구현 책임이 다르다.
  - `Acceptor`: retry/backoff, max connection guard, `Session` 생성
  - `Listener`: 경량 factory 기반 `Connection` 생성
- `Session`은 server chat domain과 강결합되어 있고,
  `Connection`은 gateway bridge transport 계층과 강결합되어 있다.

## 3. Scope Lock (Unify Targets vs Non-Targets)

### 3.1 Unify Targets (P2 대상)

1. 용어 경계 명시화
   - `Acceptor`/`Listener`와 `Session`/`Connection`의 책임 경계를 문서/코드 주석에서 명확히 통일
2. accept loop 공통 동작의 선택적 통합
   - open/bind/listen 에러 처리 패턴, stop/start 수명주기 규칙, retry 정책의 공통화 가능성 검토
3. 단계적 alias 기반 migration 경로 확보
   - 하위 호환을 유지한 상태로 점진적 이름 전환 가능하도록 설계

### 3.2 Non-Targets (P2 비대상)

1. `storage::Session` (`core/include/server/core/storage/repositories.hpp`) rename
2. Prometheus metric 이름 변경 (`chat_session_*` 등)
3. HTTP 문자열 `Connection: close` 계열 변경
4. 즉시 대규모 breaking rename (단일 커밋으로 전체 타입명 교체)

## 4. Impact Map Summary

### 4.1 `Acceptor` / `Listener`

- Definitions
  - `core/include/server/core/net/acceptor.hpp`
  - `core/include/server/core/net/listener.hpp`
- Implementations
  - `core/src/net/acceptor.cpp`
  - `core/src/net/listener.cpp`
- Construction / ownership
  - `server/src/app/bootstrap.cpp`
  - `gateway/src/gateway_app.cpp`
  - `gateway/include/gateway/gateway_app.hpp`
- Build coupling
  - `core/CMakeLists.txt`

### 4.2 `Session` / `Connection`

- Definitions
  - `core/include/server/core/net/session.hpp`
  - `core/include/server/core/net/connection.hpp`
- Server API coupling (`Session`)
  - `server/include/server/chat/chat_service.hpp`
  - `server/src/app/router.cpp`
  - `server/src/app/bootstrap.cpp`
- Gateway API coupling (`Connection`)
  - `gateway/include/gateway/gateway_connection.hpp`
  - `gateway/src/gateway_connection.cpp`
- Tests
  - `tests/core/test_core_net.cpp`
  - `tests/server/test_server_chat.cpp`

### 4.3 Docs/Knowledge coupling

- `server/README.md`
- `docs/server-architecture.md`
- `docs/core-design.md`
- `server/AGENTS.md`
- `gateway/AGENTS.md`
- `core/README.md`

## 5. Staged Rename / Integration Order

### Phase 0: Terminology alignment (doc-first)

- 문서와 AGENTS에서 현재 코드 경계(`Acceptor` vs `Listener`, `Session` vs `Connection`)를 정확히 반영

### Phase 1: Compatibility aliases (no behavior change)

- 새 역할 기반 alias 도입 (예: `ServerSession`, `TransportConnection`, `ServerAcceptor`, `GatewayListener`)
- 기존 이름은 유지하되 deprecated 전환 계획만 공지
- alias 진입점(현재 반영):
  - `core/include/server/core/net/acceptor.hpp`
    - `server::core::net::Acceptor`, `server::core::net::SessionListener`
  - `core/include/server/core/net/session.hpp`
    - `server::core::net::Session`, `server::core::net::PacketSession`
  - `core/include/server/core/net/listener.hpp`
    - `server::core::net::TransportListener`
  - `core/include/server/core/net/connection.hpp`
    - `server::core::net::TransportConnection`

### Phase 2: Consumer migration (module by module)

- server-side 사용처를 새 alias로 전환 (`bootstrap`, `router`, `chat_service`, tests)
- gateway-side 사용처를 새 alias로 전환 (`gateway_app`, `gateway_connection`, tests)

### Phase 3: Optional behavior extraction

- `Acceptor`/`Listener` 공통 accept-loop 유틸을 추출하되, 클래스 자체는 분리 유지
- retry/backoff와 factory 모델을 억지로 단일 클래스에 합치지 않음

### Phase 4: Legacy removal

- 충분한 안정화 기간 이후 legacy 이름 제거
- 제거 전 CI + smoke + distributed routing 시나리오 통과를 gate로 설정

## 6. Acceptance Criteria Before Any Breaking Rename

1. 문서/AGENTS 용어 일치 상태 확보
   - `server/AGENTS.md`, `gateway/AGENTS.md`, `docs/server-architecture.md`, `docs/core-design.md`, `core/README.md` 반영 완료
2. alias 도입 후 기존 바이너리(`server_app`, `gateway_app`, `wb_worker`) 빌드/테스트 통과
3. gateway routing + server chat path + admin control-plane smoke 통과
4. `storage::Session` 등 동명이인 영역과의 충돌 없음이 검증됨
5. legacy 이름 제거 전 include 참조가 0임을 grep으로 확인

## 7. Top Risks and Mitigations

1. Accept-loop 동작 회귀
   - 리스크: `Acceptor`의 retry/backoff/max-connection guard가 `Listener` 흐름으로 치환되며 누락될 수 있음
   - 근거 파일: `core/src/net/acceptor.cpp`, `core/src/net/listener.cpp`
   - 대응: P2에서는 동작 통합을 하지 않고 alias/용어 정리만 수행

2. `shared_from_this` 수명 관리 회귀
   - 리스크: wrapper 상속 방식으로 rename 시 비동기 콜백 수명 모델이 깨질 수 있음
   - 근거 파일: `core/include/server/core/net/acceptor.hpp`, `core/include/server/core/net/session.hpp`, `core/include/server/core/net/connection.hpp`
   - 대응: 상속 wrapper 대신 `using` alias + forwarding header 우선

3. `Session` 동명이인 충돌
   - 리스크: 네트워크 `Session`과 storage `Session`이 혼재되어 오인 가능
   - 근거 파일: `core/include/server/core/net/session.hpp`, `core/include/server/core/storage/repositories.hpp`
   - 대응: `NetSession` 같은 명시적 alias를 server/gateway 경계에 도입

4. 다중 바이너리 include break
   - 리스크: `core/include` 표면 변경 시 `server_app`/`gateway_app`/tests가 동시에 깨질 수 있음
   - 근거 파일: `server/src/app/bootstrap.cpp`, `gateway/include/gateway/gateway_app.hpp`, `tests/core/test_core_net.cpp`
   - 대응: core-first compatibility layer 도입 후 consumer 순차 마이그레이션

5. 문서/지식베이스 drift 재발
   - 리스크: 코드 rename 이후 AGENTS/architecture 문서가 늦게 반영되어 재혼선 유발
   - 근거 파일: `server/AGENTS.md`, `gateway/AGENTS.md`, `docs/server-architecture.md`, `docs/core-design.md`
   - 대응: "doc-first"를 단계 0 gate로 고정하고, rename PR마다 문서 동시 갱신 필수화

## 8. Bottom Line

P2는 "강제 단일 클래스화"가 아니라, **역할 경계 고정 + 호환 계층 기반 점진 이행**으로 진행한다.
즉시 대규모 rename 대신 doc-first -> compatibility alias -> consumer migration -> legacy removal 순서를 유지한다.

## 9. Progress Snapshot

- Phase 0 (doc-first): 완료
- Phase 1 (compat aliases): 완료
  - `server::core::net::{SessionListener, Session, PacketSession, TransportListener, TransportConnection}` alias 제공
- Phase 2 (consumer migration): 진행 중
  - gateway: backend bridge symbols are now `BackendConnection`, including `create/close_backend_connection`
  - server: `bootstrap`에서 `SessionListener`/`net::Session` 사용, `router`에서 `net::Session` alias 사용
