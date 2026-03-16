# 코어(Core) 확장 계약

## 목적
- 장기 API 거버넌스에 영향을 주는 확장 표면을 문서화합니다.
- 각 확장 계약의 성숙도와 호환성 기대치를 분류합니다.

## 현재 결정

- plugin/Lua extensibility는 app-layer convenience가 아니라 `server_core`의 **platform capability**다.
- 다만 현재 성숙도는 `Stable`이 아니라 `Transitional`이며, core mechanism과 service-specific contract를 분리해서 다룬다.
- governance 세부 규칙은 `docs/extensibility/governance.md`를 기준으로 한다.

## 현재 확장 표면

| 표면 | 위치 | 성숙도 | 비고 |
|---|---|---|---|
| Shared library + plugin host | `core/include/server/core/plugin/shared_library.hpp`, `core/include/server/core/plugin/plugin_host.hpp`, `core/include/server/core/plugin/plugin_chain_host.hpp` | Transitional | service-neutral 메커니즘 계층. chat 외 소비자 확장은 아직 후속 과제 |
| Script watcher + Lua sandbox/runtime | `core/include/server/core/scripting/script_watcher.hpp`, `core/include/server/core/scripting/lua_sandbox.hpp`, `core/include/server/core/scripting/lua_runtime.hpp` | Transitional | core 메커니즘 계층. 런타임 토글/한계/메트릭 규약이 안정화 중 |
| Chat hook plugin ABI v1/v2 | `server/include/server/chat/chat_hook_plugin_abi.hpp` | Transitional | 로더는 `chat_hook_api_v2()`를 우선 탐색하고, 미존재 시 `chat_hook_api_v1()`로 폴백합니다. |
| Chat Lua bindings | `server/src/scripting/chat_lua_bindings.cpp` | Transitional | core Lua runtime을 chat service 정책/행동으로 연결하는 첫 concrete consumer |

## Chat hook ABI v2 계약

### 엔트리포인트/호환성
- 우선 엔트리포인트: `chat_hook_api_v2()`
- 하위 호환 엔트리포인트: `chat_hook_api_v1()`
- 로더 규칙: v2 심볼이 존재하면 v2를 사용하고, 없으면 v1을 로드합니다.

### API 표면
- 공통 필드: `abi_version`, `name`, `version`, `create`, `destroy`
- v2 hook 집합:
  - `on_chat_send`
  - `on_login`
  - `on_join`
  - `on_leave`
  - `on_session_event`
  - `on_admin_command`
- `on_chat_send`는 현재 로더 validator에서 필수입니다.
- 그 외 개별 hook 포인터는 `nullptr`가 허용되며, 이 경우 해당 hook은 `kPass`와 동일하게 취급됩니다.

### 결정(Decision) 규약
- v2 결정 타입: `HookDecisionV2`
- 텍스트 변경은 `kModify`로 표현하며, 변경 결과는 체인 다음 플러그인 입력으로 전달됩니다.
- deny 계열(`kBlock`, `kDeny`)은 기본 경로를 중단합니다.
- gate 성격 hook(`on_login`, `on_join`)에서 deny가 발생하면 서버는 `MSG_ERR` (`FORBIDDEN`) + `deny_reason`을 클라이언트로 전파합니다.

### 샘플/테스트 기준
- 샘플 플러그인: `server/plugins/chat_hook_sample.cpp` (`chat_hook_sample_v2` 타깃 포함)
- 체인 회귀 테스트:
  - `tests/server/test_chat_plugin_chain_v2.cpp`
  - `tests/server/test_server_chat.cpp`

## Lua runtime capability 계약

### 표면/호환성
- `LuaRuntime` public API는 공식 빌드에서 항상 활성 capability를 제공한다.
- 운영 사용 여부는 `LUA_ENABLED`와 스크립트 디렉터리 설정이 결정한다.

### 동작 규약
- 기본 정책: 공식 배포/개발 바이너리는 Lua capability를 항상 포함하고, 기능 사용 여부는 `LUA_ENABLED`로 결정한다.
- `enabled()`는 capability 존재 여부가 아니라 런타임 객체 상태를 반영한다.
- `metrics_snapshot()`의 구조와 필드 의미는 capability 포함 빌드에서 안정적으로 유지된다.

### 테스트 기준
- 코어: `tests/core/test_lua_runtime.cpp`
- 서버 바인딩: `tests/server/test_chat_lua_bindings.cpp`

## Staged Stabilization Plan

### Stage 1 - Core mechanism hardening
- 대상: `shared_library`, `plugin_host`, `plugin_chain_host`, `script_watcher`, `lua_sandbox`, `lua_runtime`
- 요구사항:
  - core 전용 테스트로 reload/error isolation/limit/metrics 계약을 고정
  - service-specific ABI와 binding이 core header에 스며들지 않도록 유지
  - 여전히 `Transitional`로 유지

### Stage 2 - Service contract hardening
- 대상: chat hook ABI v1/v2, chat plugin chain, chat Lua bindings
- 요구사항:
  - deny/modify/handled semantics, fallback order, auto-disable, conflict-policy 문서를 테스트와 함께 고정
  - sample plugin/script와 control-plane 문서를 같은 규칙으로 유지

### Stage 3 - External consumer proof
- 대상: 향후 gateway/wb_worker 또는 out-of-tree consumer
- 요구사항:
  - non-chat consumer 또는 package-consumer 검증 추가
  - migration-note discipline과 compatibility evidence가 쌓인 뒤에만 일부 core mechanism을 `Stable` 후보로 검토

## 확장 ABI 거버넌스 규칙
- 확장 ABI 변경은 PR 설명에서 호환/파괴 변경으로 분류해야 합니다.
- 파괴적 ABI 변경은 머지 전에 `docs/core-api/` 하위 마이그레이션 노트가 필요합니다.
- ABI 형태를 변경하는 PR은 동일 PR에서 확장 ABI 문서를 함께 갱신해야 합니다.
- 플러그인 로더 동작 변경은 운영 안전성(lock/sentinel, reload 의미)을 유지하거나 명시적 마이그레이션 가이드를 포함해야 합니다.
- Lua host API 또는 `ctx` 필드 의미를 깨는 변경도 동일한 수준의 마이그레이션 노트를 요구합니다.

## Future Candidate Surfaces

이 항목들은 현재 구현된 공개 계약이 아니라, 향후 확장 가능성 후보를 추적하기 위한 메모다.
active docs tree에서는 별도 leaf 문서를 두지 않고 여기서만 유지한다.

### Gateway-side candidates

- routing policy hook
  - frontend session metadata와 backend 후보 집합을 받아 선택 backend를 반환하는 후보
- admission policy hook
  - 연결 메타데이터를 받아 allow/deny와 사유 코드를 반환하는 후보
- session lifecycle hook
  - connect/disconnect/reconnect 이벤트에 대한 관측/정책 후보

원칙:

- 요청 경로에서 실행되는 훅은 논블로킹이어야 한다.
- 실패/타임아웃 시 gateway는 내장 기본 정책으로 폴백해야 한다.
- Stable 승급 전에는 입력 DTO, 결과 스키마, timeout 의미를 고정해야 한다.

### Write-behind candidates

- pre-write transform hook
  - 디코딩된 stream event를 변환하는 후보
- filter hook
  - DB write 전 keep/drop 결정을 내리는 후보
- failure classification hook
  - write/validation 실패를 retry/DLQ/drop 정책으로 매핑하는 후보

원칙:

- 훅 실패 시 기존 write-behind 오류 정책(`WB_ACK_ON_ERROR`, DLQ 설정)으로 보수적 폴백을 유지해야 한다.
- drop/fallback 결정은 이유 태그가 포함된 관측 신호를 남겨야 한다.
- Stable 승급 전에는 고정 event DTO와 결과 스키마가 필요하다.

## 이번 단계 비목표
- 새 런타임 확장 메커니즘 구현 없음
- 프로토콜 레벨 재설계 없음

## 배포 정책 메모 (Plugin/Lua/Protocol)
- 공식 배포 빌드는 plugin/Lua/protocol capability를 포함하는 구성을 기본으로 한다.
- 사용 여부는 런타임 설정(예: `CHAT_HOOK_ENABLED`, `LUA_ENABLED`) 또는 요청 경로(opcode/wire dispatch)에 의해 결정한다.
- Lua 샘플/온보딩 문서는 function-style hook + `ctx`를 기본 모델로 삼고, directive/return-table은 fallback/testing aid로만 취급한다.
