# Plugin/Script Test Plan

기준 문서:
- `tasks/runtime-extensibility-todo.md`
- `docs/runtime-extensibility-plan.md`

## 1. Goal

- 네이티브 플러그인(chat hook)과 스크립팅(Lua 예정)의 기능/안정성/성능을 단계적으로 검증한다.
- 현재 Docker 기반 smoke/E2E 검증은 유지하고, 빠른 회귀를 위한 core/server 단위 테스트를 보강한다.
- CI에서 빠른 게이트와 운영형 통합 게이트를 분리한다.

## 2. Baseline

현재 자동 검증:
- CI Docker E2E에서 플러그인 메트릭 확인 (`Verify Plugin Metrics`)
- CI Docker E2E에서 v2 swap/hot-reload 반영 확인 (`Hot Reload Plugin (v2) And Verify Metrics`)

현재 공백:
- `PluginHost`, `PluginChainHost`, `ScriptWatcher` 전용 단위 테스트 부재
- v1/v2 혼합 체인/호환 폴백 자동 검증 부재
- Lua runtime/sandbox/script hot-reload 자동 검증 부재

## 3. Test Layers

- L0 Unit (core)
- L1 Component (server)
- L2 Integration (docker + python)
- L3 E2E/Soak (운영형 부하/회귀)
- L4 Resilience/Security (실패 흡수, 제한, 롤백)

## 4. Scenario Matrix

### A. Native Plugin (Phase 1~2)

L0 Unit (`tests/core/`):
- `P-U-001` PluginHost 기본 로드/심볼 조회 성공
- `P-U-002` 엔트리포인트/validator 실패 시 기존 플러그인 유지
- `P-U-003` mtime 불변 시 reload 스킵
- `P-U-004` lock/sentinel 존재 시 reload 스킵
- `P-U-005` reload metrics(`attempt/success/failure`) 증가 검증
- `P-U-006` broken swap 후 read path에서 직전 정상 인스턴스 유지

L0 Unit (`tests/core/`):
- `P-C-001` PluginChainHost 파일명 순서 로드 (`10_`, `20_`)
- `P-C-002` 추가/삭제 감지 후 체인 갱신
- `P-C-003` 스캔 실패 시 기존 체인 유지
- `P-C-004` (Phase2) v1/v2 혼합 로드 및 v1 폴백 검증

L1 Component (`tests/server/`):
- `P-S-001` 결정 우선순위 `kBlock > kHandled > kReplaceText > kPass`
- `P-S-002` v1 단독/복수 체인 결과 일관성
- `P-S-003` (Phase2) v2 hook 거부/허용 분기 (`on_login`, `on_join`, `on_leave`, `on_session_event`, `on_admin_command`)
- `P-S-004` v1+v2 혼합 체인 호출 순서/결정 일관성

L2 Integration (`tests/python/` + docker stack):
- `P-I-001` 두 서버 인스턴스 모두 plugin metrics 노출 확인
- `P-I-002` lock -> swap(v1->v2) -> unlock 후 version label 전환 확인
- `P-I-003` swap 실패(손상 파일) 시 서비스 생존 + failure metric 증가
- `P-I-004` rollback(v2->v1) 정상 복귀

### B. ScriptWatcher/Lua (Phase 3~4)

L0 Unit (`tests/core/`):
- `S-U-001` 확장자 필터
- `S-U-002` recursive on/off 범위
- `S-U-003` lock_path 존재 시 poll 스킵
- `S-U-004` add/modify/remove 이벤트 감지
- `S-U-005` 이벤트 정렬 안정성

L0 Unit (`tests/core/`):
- `S-L-001` LuaRuntime load/call/reset
- `S-L-002` LuaSandbox 금지 라이브러리 차단
- `S-L-003` instruction limit 초과 처리 (scaffold directive `limit=instruction`)
- `S-L-004` memory limit 초과 처리 (scaffold directive `limit=memory`)

L1 Component (`tests/server/`):
- `S-S-001` 네이티브 `kBlock/kDeny` 시 Lua 미호출
- `S-S-002` 네이티브 `kPass` 후 Lua 결정 반영
- `S-S-003` hot path(`on_chat_send`) Lua 차단
- `S-S-004` auto-disable -> reload 재활성화

L2 Integration (`tests/python/` + docker stack):
- `S-I-001` `.lua` 수정 후 hot-reload 반영
- `S-I-002` 연속 실패 스크립트 자동 비활성화 + metrics
- `S-I-003` 정상 스크립트 재배포 후 재활성화
- `S-I-004` primary/fallback 스크립트 디렉터리 전환 감지 (`/app/scripts` <-> `/app/scripts_builtin`)

### C. Metrics/Perf/Resilience (Phase 4~6)

- `M-P-001` plugin/lua metrics 노출 + label 무결성
- `M-P-002` hook latency histogram 수집
- `M-P-003` 플러그인 유/무 p99 회귀 가드
- `M-P-004` Lua cold path SLA(<1ms 목표)
- `M-P-005` 오류 주입(예외/timeout/memory) 시 프로세스 생존성

## 5. File/Target Mapping

신규 C++ 테스트 파일(예정):
- `tests/core/test_plugin_host.cpp`
- `tests/core/test_plugin_chain_host.cpp`
- `tests/core/test_script_watcher.cpp`
- `tests/core/test_lua_runtime.cpp`
- `tests/core/test_lua_sandbox.cpp`
- `tests/server/test_chat_plugin_chain_v2.cpp`
- `tests/server/test_lua_hook_integration.cpp`
- `tests/server/test_hook_auto_disable.cpp`

신규 Python 통합 테스트(예정):
- `tests/python/verify_plugin_hot_reload.py`
- `tests/python/verify_plugin_rollback.py`
- `tests/python/verify_script_hot_reload.py`
- `tests/python/verify_script_fallback_switch.py`
- `tests/python/verify_lua_auto_disable.py`

보강 대상:
- `tests/CMakeLists.txt` (타깃 + `gtest_discover_tests`)
- `.github/workflows/ci.yml` (plugin/script step 분리)

## 6. CI Plan

1) Fast Gate (PR 기본)
- Windows: `ctest --preset windows-test` + 신규 core/server 단위 테스트 포함
- Linux: 계약/코드젠/문서 + 핵심 단위 테스트

2) Integration Gate (PR/merge)
- Docker stack + plugin hot-reload + metrics
- (Phase3 이후) script hot-reload + sandbox + auto-disable

3) Matrix Gate
- `BUILD_LUA_SCRIPTING=OFF` (기존 동작 불변)
- `BUILD_LUA_SCRIPTING=ON` (Lua 경로 전용)

## 7. Recommended Execution Order

1. `PluginHost`, `PluginChainHost`, `ScriptWatcher` 단위 테스트
2. `chat_plugin_chain_v2`, `lua_hook_integration` 컴포넌트 테스트
3. Python Docker 시나리오 `reload/rollback`
4. Lua 활성 후 `script hot-reload/auto-disable`
5. 성능 게이트(p95/p99/throughput) 수치 고정

## 8. DoD

- 네이티브 플러그인 로드/리로드/lock/sentinel/rollback/metrics 자동 검증
- 스크립트 watcher/reload/sandbox/auto-disable 자동 검증
- `BUILD_LUA_SCRIPTING` ON/OFF 양 경로 회귀 없음
- CI 실패 시 계층(L0~L4) 단위로 원인 구간 즉시 식별 가능
