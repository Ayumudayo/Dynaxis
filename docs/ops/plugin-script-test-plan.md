# Plugin/Script Test Plan

기준 문서:
- `docs/runtime-extensibility-plan.md`

## 1. Goal

- 네이티브 플러그인(chat hook)과 스크립팅(Lua cold-hook)의 기능/안정성/성능을 단계적으로 검증한다.
- 현재 Docker 기반 smoke/E2E 검증은 유지하고, 빠른 회귀를 위한 core/server 단위 테스트를 보강한다.
- CI에서 빠른 게이트와 운영형 통합 게이트를 분리한다.

## 2. Baseline

현재 자동 검증:
- CI Docker stack 경로에서 ctest label(`plugin-script`) 기반 plugin/script smoke 실행
- 개별 시나리오(`verify_plugin_hot_reload`, `verify_plugin_v2_fallback`, `verify_plugin_rollback`, `verify_script_hot_reload`, `verify_script_fallback_switch`, `verify_chat_hook_behavior`)를 하나의 게이트로 집계
- `LuaRuntimeTest`/`LuaSandboxTest`/`ChatLuaBindingsTest`는 capability 포함 기본 CI 경로에서 회귀 검증
- 문서/샘플 기준 작성 모델은 function-style hook + `ctx`이며, directive/return-table은 fallback/testing aid로만 유지

현재 공백:
- stack 의존 Python 테스트는 `KNIGHTS_ENABLE_STACK_PYTHON_TESTS=1` 환경이 있어야 실행됨
- runtime toggle matrix는 CI baseline(OFF) + smoke(ON)에서 고정되지만, 향후 `CHAT_HOOK_ENABLED/LUA_ENABLED` 조합(4개) 전체를 자동화할 필요가 있음

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
- `S-L-003` instruction limit 초과 처리 (현재 fallback directive `limit=instruction` 기반)
- `S-L-004` memory limit 초과 처리 (현재 fallback directive `limit=memory` 기반)

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
- `.github/workflows/ci-extensibility.yml` (plugin/script stack 게이트)

## 6. CI Plan

1) Fast Gate (PR 기본)
- Windows: `ctest --preset windows-test` + Lua 핵심 테스트군(`LuaRuntimeTest|LuaSandboxTest|ChatLuaBindingsTest`)
- Linux: 계약/코드젠/문서 + 핵심 단위 테스트 + stack baseline/runtime 토글 검증

2) Integration Gate (PR/merge)
- Docker stack + plugin hot-reload + metrics
- (Phase3 이후) script hot-reload + sandbox + auto-disable

3) Matrix Gate
- Capability Gate: 공식 아티팩트는 Lua capability를 항상 포함
- Runtime Gate: `CHAT_HOOK_ENABLED=0/1`, `LUA_ENABLED=0/1` 시나리오 회귀

현재 CI 반영:
- baseline(OFF): stack를 기본 env(`CHAT_HOOK_ENABLED=0`, `LUA_ENABLED=0`)로 기동 후 `verify_runtime_toggle_metrics.py`로 두 토글이 비활성 상태인지 확인
- baseline(OFF) protocol continuity: `verify_pong.py` + `verify_chat.py`를 함께 실행해 토글 비활성 상태에서도 기본 프로토콜/채팅 경로가 유지되는지 확인
- smoke(ON): stack를 `CHAT_HOOK_ENABLED=1`, `LUA_ENABLED=1`로 기동 후 동일 스크립트로 활성 상태 확인

권장 실행 명령(Windows):

```powershell
# capability 포함 기본 경로
pwsh scripts/build.ps1 -Config Debug -Target core_plugin_runtime_tests
ctest --test-dir build-windows -C Debug -R "LuaRuntimeTest|LuaSandboxTest|ChatLuaBindingsTest" --output-on-failure --no-tests=error

# runtime 토글 점검 (stack 필요)
python tests/python/verify_runtime_toggle_metrics.py --expect-chat-hook-enabled 0 --expect-lua-enabled 0
python tests/python/verify_runtime_toggle_metrics.py --expect-chat-hook-enabled 1 --expect-lua-enabled 1
```

검증 규칙:
- Lua 관련 테스트는 실제로 매치/실행되어야 하며, 0개 매치 시 `--no-tests=error`로 실패 처리한다.
- 공식 빌드 경로는 Lua capability가 포함된 바이너리와 핵심 테스트를 통해 검증한다.
- 런타임 제어는 `CHAT_HOOK_ENABLED`/`LUA_ENABLED` on/off 시나리오에서 기능 우회/활성이 모두 확인되어야 한다.

4) Doxygen/문서 게이트
- `python tools/check_doxygen_coverage.py`
- `ctest --test-dir build-windows -C Debug -R DoxygenCoverageToolTests --output-on-failure`

## 7. Recommended Execution Order

1. `PluginHost`, `PluginChainHost`, `ScriptWatcher` 단위 테스트
2. `chat_plugin_chain_v2`, `lua_hook_integration` 컴포넌트 테스트
3. Python Docker 시나리오 `reload/rollback`
4. Lua 활성 후 `script hot-reload/auto-disable`
5. 성능 게이트(p95/p99/throughput) 수치 고정

## 8. DoD

- 네이티브 플러그인 로드/리로드/lock/sentinel/rollback/metrics 자동 검증
- 스크립트 watcher/reload/sandbox/auto-disable 자동 검증
- 공식 빌드(항상 capability 포함) + 런타임 토글(`CHAT_HOOK_ENABLED`, `LUA_ENABLED`) 회귀 없음
- CI 실패 시 계층(L0~L4) 단위로 원인 구간 즉시 식별 가능
