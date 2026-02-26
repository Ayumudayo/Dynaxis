# 코어(Core) 확장 계약

## 목적
- 장기 API 거버넌스에 영향을 주는 확장 표면을 문서화합니다.
- 각 확장 계약의 성숙도와 호환성 기대치를 분류합니다.

## 현재 확장 표면

| 표면 | 위치 | 성숙도 | 비고 |
|---|---|---|---|
| Chat hook plugin ABI | `server/include/server/chat/chat_hook_plugin_abi.hpp` | Transitional | 명시적 플러그인 진입 계약이 존재하며, hot-reload manager/chain은 `server/src/chat/chat_hook_plugin_manager.*`, `server/src/chat/chat_hook_plugin_chain.*`에 구현되어 있습니다. |

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
