# CI / Runtime Capability Simplification Plan

상태: Phase A/B 완료, Phase C 1차 workflow split 구현 완료

작성일: 2026-03-06

## 1. 목적

현재 Knights는 다음 두 문제가 동시에 존재한다.

1. capability 포함 여부와 기능 사용 여부가 build flag와 runtime toggle에 나뉘어 있어 정책이 일관되지 않다.
2. `.github/workflows/ci.yml` 한 파일에 빠른 회귀, core API governance, Docker stack smoke, plugin/script 통합 검증, hardening 성격 검증, 캐시/prefetch 성격 관심사가 과도하게 섞여 있다.

이 문서의 목표는 다음과 같다.

- capability는 기본 빌드에 항상 포함하고, 실제 사용 여부는 런타임 설정으로만 제어한다.
- 제품 변형이 아닌 과도기 검증 용도로 남아 있는 build flag를 제거 대상으로 전환한다.
- required CI는 "빠른 신뢰도 높은 검증"만 수행하고, 시간이 오래 걸리거나 운영형 성격이 강한 검증은 별도 workflow로 분리한다.

## 2. 문제 진단

### 2.1 Capability 정책의 혼선

현재 문서와 운영 모델은 이미 다음 정책을 암묵적으로 채택하고 있다.

- 공식 배포/개발 바이너리는 Lua capability를 포함한 상태로 제공한다.
- 실제 기능 활성화는 `CHAT_HOOK_ENABLED`, `LUA_ENABLED`, `GATEWAY_UDP_LISTEN` 같은 런타임 설정이 결정한다.

작업 시작 시 빌드 그래프와 CI에는 아래 build flag가 남아 있었다.

- `BUILD_LUA_SCRIPTING`
- `KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS`

이 둘은 현재 제품 라인업을 나누는 필수 build matrix라기보다, 과도기 구현 검증과 source-selection regression을 위해 남아 있는 성격이 강했다.
2026-03-06 기준으로 두 build flag와 관련 off-build 경로는 코드/도커/CI에서 제거 완료했다.

### 2.2 CI 구조의 과응집

현재 `ci.yml`은 대략 아래 관심사를 함께 떠안고 있다.

- Windows dependency/cache priming
- Windows fast unit/regression
- Core API governance / consumer tests
- Linux build variants
- ASan + fuzz
- Docker baseline/off + runtime on stack smoke
- plugin/script hot-reload / fallback / rollback
- admin/control-plane / soak gate

이 구조는 다음 문제를 만든다.

- required PR gate가 느리고 원인 구분이 어렵다.
- path와 무관한 job까지 항상 같이 돈다.
- "검증"과 "운영형 rehearsal", "캐시 최적화", "PoC"가 한 파일에 섞여 유지보수성이 낮다.

## 3. 정책 결정

### 3.1 런타임 제어 유지 대상

다음 런타임 설정은 유지한다.

- `CHAT_HOOK_ENABLED`
  - 네이티브 plugin chain의 독립 kill-switch
- `LUA_ENABLED`
  - Lua runtime / watcher / script load path의 독립 kill-switch
- `GATEWAY_UDP_LISTEN`
  - UDP ingress socket bind 여부를 결정하는 runtime bind config

이 세 값은 capability 포함 여부가 아니라 운영 중 사용 여부와 rollout 범위를 제어한다.

### 3.2 제거 대상 build flag

다음 build flag는 제거 대상으로 본다.

- `BUILD_LUA_SCRIPTING`
- `KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS`

원칙:

- `server_core`는 Lua capability를 항상 포함한 상태로 빌드한다.
- `gateway_app`는 UDP ingress capability를 항상 포함한 상태로 빌드한다.
- 기능 사용 여부는 runtime setting으로만 결정한다.

### 3.3 CI 정책

required PR gate는 다음 성격만 남긴다.

- codegen / schema freshness
- 핵심 unit/regression
- 필수 governance / contract
- baseline Docker smoke

다음 성격은 분리 대상으로 본다.

- ASan / UBSan
- fuzz
- soak / perf
- plugin/script full stack rehearsal
- cache prewarm / PoC workflow

## 4. 목표 상태

### 4.1 Build / Runtime

목표 상태:

- Lua capability는 기본 빌드에 항상 포함된다.
- UDP ingress capability는 기본 빌드에 항상 포함된다.
- capability 사용 여부는 런타임 설정만으로 제어한다.
- `lua_runtime_disabled.cpp`, lua-off preset, source-selection regression은 제거 완료 상태로 유지한다.

비목표:

- RUDP rollout/runtime setting 제거
- `CHAT_HOOK_ENABLED`와 `LUA_ENABLED`의 즉시 통합
  - 둘은 당분간 독립 kill-switch로 유지한다.

### 4.2 CI

목표 상태:

- required PR workflow는 빠르고 원인 구분이 쉽다.
- 운영형 통합 검증과 hardening 검증은 별도 workflow로 분리된다.
- path와 무관한 무거운 검증은 항상 돌지 않는다.

## 5. 제안 Workflow 구조

### 5.1 `ci-fast.yml`

용도:

- PR required gate

포함:

- codegen/spec freshness
- Doxygen/tool unit tests
- Windows 또는 Linux 중 한 쪽의 표준 unit/regression
- runtime capability 기본 검증(`CHAT_HOOK_ENABLED=0`, `LUA_ENABLED=0` baseline 일부 포함 가능)

### 5.2 `ci-api-governance.yml`

용도:

- core public API / stable governance 전용

포함:

- boundary / stable governance / doc freshness / consumer tests

정책:

- API 관련 path에서만 required 또는 path-gated 실행

### 5.3 `ci-stack.yml`

용도:

- Docker baseline/off + runtime on 기본 stack smoke

포함:

- health/ready
- ping/chat
- load balancing
- cross-instance whisper
- admin API/auth/control-plane 기본 smoke

### 5.4 `ci-extensibility.yml`

용도:

- plugin/script full stack smoke

포함:

- plugin metrics / hot-reload / rollback / staged-v2 baseline check
- Lua hot-reload / fallback switch
- chat hook behavior

정책:

- plugin/script 관련 path 또는 runtime extensibility 관련 path에서 실행

### 5.5 `ci-hardening.yml`

용도:

- merge queue / main / nightly

포함:

- ASan / UBSan
- fuzz
- soak / perf
- 장시간/고비용 validation

### 5.6 `ci-prewarm.yml`

용도:

- schedule / workflow_dispatch only

포함:

- Conan cache prewarm
- base image prewarm
- optional experimental cache telemetry

## 6. 단계별 이행 계획

### Phase A - 정책 고정

작업:

- build capability vs runtime toggle 정책을 문서로 고정
- 유지할 runtime setting과 제거할 build flag를 확정
- 관련 문서 용어를 통일

완료 기준:

- `docs/configuration.md`, runtime/extensibility 문서, README가 동일 정책을 설명한다.

### Phase B - Build Graph 단순화

작업:

- `BUILD_LUA_SCRIPTING` 제거
- `KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS` 제거
- Lua/UDP capability를 항상 포함하는 빌드 그래프로 전환
- 관련 disabled path / off preset / checker 제거 또는 축소

완료 기준:

- capability 항상 포함 빌드에서 `CHAT_HOOK_ENABLED`, `LUA_ENABLED`, `GATEWAY_UDP_LISTEN`만으로 on/off가 제어된다.

진행 상태:

- 완료 (2026-03-06)

### Phase C - CI 분리

작업:

- `ci.yml`에서 fast / governance / stack / extensibility / hardening / prewarm 역할을 분리
- required gate와 optional/nightly gate를 재분류
- path filter 도입

완료 기준:

- PR required workflow 수와 책임이 명확하고, 실패 원인 계층이 바로 드러난다.

진행 상태:

- 1차 구현 완료 (2026-03-06)
- 분리 결과:
  - `ci-fast.yml`
  - `ci-api-governance.yml`
  - `ci-stack.yml`
  - `ci-extensibility.yml`
  - `ci-hardening.yml`
  - `ci-prewarm.yml`
- 기존 단일 `.github/workflows/ci.yml`는 제거했다.
- 남은 운영 작업:
  - GitHub branch protection / required check 이름을 새 workflow 이름 기준으로 재설정
  - path filter 범위와 required 여부를 실제 CI telemetry 기준으로 추가 미세조정

### Phase D - 검증 재정의

작업:

- build variant 검증을 runtime off/on 검증으로 치환
- plugin-only / Lua-only / both-on 조합의 최소 검증 범위 결정
- 운영형 smoke와 장시간 hardening 검증의 주기를 분리

완료 기준:

- CI가 실제 제품 전략을 검증하고, 과도기 변형 빌드에 불필요한 비용을 쓰지 않는다.

진행 상태:

- 검증 정책 문서화 완료
- 실제 workflow 파일 분리 및 required gate 재분류는 후속 작업

## 7. 영향 범위

예상 영향 파일:

- 루트 `CMakeLists.txt`
- 루트 `README.md`
- `Dockerfile`
- `core/CMakeLists.txt`
- `gateway/CMakeLists.txt`
- `core/src/scripting/lua_runtime_disabled.cpp`
- `core/include/server/core/scripting/lua_runtime.hpp`
- `server/src/app/bootstrap.cpp`
- `CMakePresets.json`
- `.github/workflows/*.yml`
- `tools/check_lua_build_toggle.py`
- `docker/stack/scripts/on_login_welcome.script.json`
- `docs/configuration.md`
- `docs/runtime-extensibility-plan.md`
- `docs/ops/plugin-script-test-plan.md`
- `docs/core-api/extensions.md`
- `docs/extensibility/lua-quickstart.md`
- `server/README.md`
- `tests/core/test_lua_runtime.cpp`
- `tests/core/test_script_watcher.cpp`
- `tests/server/test_chat_lua_bindings.cpp`
- `tests/server/test_hook_auto_disable.cpp`
- `tests/server/test_lua_hook_integration.cpp`
- `tests/server/test_server_chat.cpp`

## 7.1 현재 구현 결과 (2026-03-06)

- `BUILD_LUA_SCRIPTING`, `KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS`를 코드/도커/CI에서 제거했다.
- LuaJIT/sol2 vendor helper는 capability-always-on 모델로 수정해, 깨끗한 configure에서도 Lua vendor target이 항상 생성되도록 정리했다.
- `KNIGHTS_BUILD_LUA_SCRIPTING` 호환 매크로와 dead test branch를 제거해 Lua 테스트를 항상-capability 기준으로 단순화했다.
- Windows fast CI의 중복 Lua ctest 재실행을 제거했다.
- 기존 단일 `ci.yml`을 `ci-fast`, `ci-api-governance`, `ci-stack`, `ci-extensibility`, `ci-hardening`, `ci-prewarm`으로 분리하고 역할별 trigger를 재설정했다.
- prewarm 성격 job은 PR gate에서 분리하고 schedule / `workflow_dispatch` 전용 workflow로 옮겼다.
- hardening 성격 검증(ASan/fuzz/soak)은 PR 기본 gate에서 분리하고 `main` / `merge_group` / nightly 경로로 이동했다.
- 검증:
  - `pwsh scripts/build.ps1 -Config Release`
  - `ctest --preset windows-test --output-on-failure`
  - `scripts/deploy_docker.ps1 -Action up -Detached -Build`
  - baseline/off: `verify_runtime_toggle_metrics.py`, `verify_pong.py`, `verify_chat.py`
  - runtime on: `verify_runtime_toggle_metrics.py`, `verify_script_hot_reload.py`, `verify_chat_hook_behavior.py`, `verify_plugin_hot_reload.py --check-only`
  - workflow YAML parse: `.github/workflows/*.yml`

## 8. 리스크와 완화

### 리스크 1 - capability 항상 포함 시 의존성/패키징 부담 증가

완화:

- 공식 배포 artifact가 실제로 단일 capability 포함 모델인지 먼저 확인한다.
- 외부 consumer가 capability 없는 core를 진짜 필요로 하면 전역 build flag 대신 별도 모듈/컴포넌트 경계로 재설계한다.

### 리스크 2 - required gate 재구성 중 coverage 공백

완화:

- workflow 분리는 "실행 내용 유지, 파일/경계만 먼저 분리" 순서로 진행한다.
- merge 전 일정 기간 old/new workflow를 병행하거나, 최소한 main/nightly에 hardening gate를 유지한다.

### 리스크 2.1 - branch protection drift

완화:

- 기존 required check가 `CI` 단일 workflow 이름에 묶여 있었다면, 새 workflow 이름(`CI Fast`, `CI API Governance`, `CI Stack` 등) 기준으로 GitHub repository settings를 갱신한다.
- merge 전 실제 PR check list에서 required/optional 구성이 의도대로 보이는지 확인한다.

### 리스크 3 - plugin/script 검증의 path filter 누락

완화:

- 첫 단계에서는 path filter를 보수적으로 넓게 잡고, telemetry를 본 뒤 축소한다.

## 9. 롤백 기준

다음 상황이면 구조 리팩터링을 중단하거나 이전 단계로 되돌린다.

- required PR gate의 실패 원인 구분이 이전보다 더 어려워질 때
- runtime-only gating 전환 후 실제 capability 누락/packaging 누락이 발견될 때
- workflow 분리 후 execution gap 때문에 main에서 회귀가 발생할 때
- branch protection이 새 workflow 이름으로 갱신되지 않아 merge gate가 깨질 때

## 10. Open Questions

- `CHAT_HOOK_ENABLED`와 `LUA_ENABLED`를 장기적으로 하나의 상위 `EXTENSIBILITY_ENABLED` 아래 둘지 여부
- core public API governance를 항상 required로 둘지, path-gated로 내릴지 여부
- plugin/script full stack smoke를 PR required로 유지할지, merge/nightly로 내릴지 여부
- ASan/fuzz/soak를 merge queue에 둘지 nightly에 둘지 여부

## 11. 권장 실행 순서

1. 정책 문서/README/configuration 정리
2. build flag 제거 전 영향 파일 및 제거 경로 목록 확정
3. CI workflow 분리안 초안 작성
4. capability always-on build graph 전환
5. runtime-only gating으로 테스트/문서 정리
6. required / optional / nightly gate 재분류 확정
