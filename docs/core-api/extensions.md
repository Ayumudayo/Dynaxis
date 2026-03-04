# 코어(Core) 확장 계약

## 목적
- 장기 API 거버넌스에 영향을 주는 확장 표면을 문서화합니다.
- 각 확장 계약의 성숙도와 호환성 기대치를 분류합니다.

## 현재 확장 표면

| 표면 | 위치 | 성숙도 | 비고 |
|---|---|---|---|
| Chat hook plugin ABI v1/v2 | `server/include/server/chat/chat_hook_plugin_abi.hpp` | Transitional | 로더는 `chat_hook_api_v2()`를 우선 탐색하고, 미존재 시 `chat_hook_api_v1()`로 폴백합니다. |

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
- 개별 hook 포인터는 `nullptr`가 허용되며, 이 경우 해당 hook은 `kPass`와 동일하게 취급됩니다.

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

## 확장 ABI 거버넌스 규칙
- 확장 ABI 변경은 PR 설명에서 호환/파괴 변경으로 분류해야 합니다.
- 파괴적 ABI 변경은 머지 전에 `docs/core-api/` 하위 마이그레이션 노트가 필요합니다.
- ABI 형태를 변경하는 PR은 동일 PR에서 확장 ABI 문서를 함께 갱신해야 합니다.
- 플러그인 로더 동작 변경은 운영 안전성(lock/sentinel, reload 의미)을 유지하거나 명시적 마이그레이션 가이드를 포함해야 합니다.

## 다음 계약 후보(설계 목표)
- Gateway 확장 인터페이스 설계: `docs/core-api/gateway-extension-interface.md`
- Write-behind 확장 인터페이스 설계: `docs/core-api/write-behind-extension-interface.md`

## 이번 단계 비목표
- 새 런타임 확장 메커니즘 구현 없음
- 프로토콜 레벨 재설계 없음
