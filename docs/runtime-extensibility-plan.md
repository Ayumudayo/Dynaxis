# 런타임 확장성 구현 계획

> 목적: 서버 스택(server_app, gateway_app, wb_worker)이 런타임에 동적으로 동작을 변경할 수 있도록, **네이티브 플러그인(C ABI)**과 **Lua 스크립팅**을 하이브리드로 도입하는 구현 계획을 정의한다.

> 상태: 실행 중 (Stream A 완료, Stream B 대기)

## 최근 구현 반영 (2026-03-05)

- Phase 6.5 CI 단순화:
  - plugin/script Python smoke를 개별 step 나열 대신 ctest label(`plugin-script`) 기반으로 집계했다.
  - stack 의존 Python 테스트는 `KNIGHTS_ENABLE_STACK_PYTHON_TESTS=1`일 때만 실행되고, 미설정 시 skip code(77)로 처리한다.
  - Docker stack 기본값은 `CHAT_HOOK_ENABLED=0`, `LUA_ENABLED=0`으로 두고, CI plugin/script smoke에서는 두 토글을 명시적으로 `1`로 설정해 활성 시나리오를 검증한다.
  - `tools/check_lua_build_toggle.py`를 추가해 `BUILD_LUA_SCRIPTING` cache 토글과 Lua runtime source 선택(`lua_runtime.cpp` vs `lua_runtime_disabled.cpp`)을 자동 검증한다.
  - Linux CI는 기본 경로를 `BUILD_LUA_SCRIPTING=ON`으로 고정하고, `--require-source-check` + `ctest --no-tests=error`로 Lua capability 포함/검증을 강제한다.
  - Windows fast-tests도 기본 빌드(`build-windows`)에서 `tools/check_lua_build_toggle.py --expect on` + Lua 관련 테스트(`LuaRuntimeTest|LuaSandboxTest|ChatLuaBindingsTest`)를 강제한다.
- Phase 3 build-toggle 경로 보강:
  - `core/CMakeLists.txt`에서 `BUILD_LUA_SCRIPTING` 값에 따라 `lua_runtime.cpp`(ON) 또는 `lua_runtime_disabled.cpp`(OFF)를 선택하도록 분리했다.
  - OFF 빌드에서 `LuaRuntime` API 시그니처는 유지하고, disabled-mode 결과를 반환하는 경로를 고정했다.
  - `tests/core/test_lua_runtime.cpp`에 `enabled()` 분기 및 OFF error counter 동작을 추가해 ON/OFF 패리티를 명시적으로 잠갔다.
  - `CMakePresets.json`에 `windows-lua-off`/`windows-lua-off-debug`/`windows-lua-off-test` 프리셋을 추가해 OFF 회귀 명령을 표준화했다.
- Phase 4.5 관측성:
  - Grafana `chat-server-runtime` 대시보드에 `Extensibility` row를 추가했다.
  - 패널: reload 성공률, hook 호출 빈도, hook 에러율, Lua 메모리 사용량, auto-disable 이벤트.
- Phase 4.6 검증:
  - `tests/server/test_hook_auto_disable.cpp`에 Lua limit 실패(instruction/memory) 시 관리자 경로가 계속 정상 동작함을 검증하는 테스트를 추가했다.
  - auto-disable 임계 검증을 3회 연속 실패 기준으로 보강하고, reload 후 재활성화를 확인했다.
  - `/metrics`에서 `chat_frame_total`, `hook_auto_disable_total`, `lua_script_calls_total`, `lua_memory_used_bytes` 노출을 확인했다.
- 문서/온보딩:
  - `docs/extensibility/plugin-quickstart.md`
  - `docs/extensibility/lua-quickstart.md`
  - `docs/extensibility/conflict-policy.md`
  - `docs/extensibility/recipes.md`

참고: 현재 Lua 경로에는 스캐폴드 기반 limit 시뮬레이션(`limit=instruction|memory`) 검증이 포함된다.

---

## 0. 미완료 항목 실행 계획

아래 계획은 현재 미완료 backlog를 우선순위 기준으로 묶어, 리뷰/검증 가능한 단위로 순차 완료하기 위한 실행안이다.

### 0.1 우선순위 및 완료 기준

| 우선순위 | 범위 | 완료 기준 |
|---|---|---|
| P0 | Phase 2 잔여 항목 마감 (ABI v2 운영 준비) | v2 샘플 플러그인 + deny reason 검증 + 문서 동기화 완료 |
| P1 | Phase 3 기반 구축 (Lua 런타임/샌드박스) | 공식 빌드(`BUILD_LUA_SCRIPTING=ON`) + 런타임 토글(`LUA_ENABLED`) 회귀 통과 |
| P2 | 서버 통합/리로드/운영 가시성 | cold path Lua 체인 + hot-reload + 메트릭 노출 완료 |
| P3 | 운영화 (Docker/CI/문서/가이드) | Docker/CI 재현 가능 + 온보딩 문서 완비 |

### 0.2 실행 스트림

#### Stream A - Phase 2 마감 (P0)

완료 상태(2026-03-04, `feature/Plugin-Script`):
- [x] `server/plugins/chat_hook_sample.cpp`를 `ChatHookApiV2` 기준으로 승격하고 v1 동작 호환을 유지함
- [x] `on_join` deny 시 `deny_reason` 클라이언트 전달 통합 테스트 추가/검증 완료
- [x] ABI/사용법 문서 동기화 완료 (`server/README.md`, `docs/core-api/extensions.md`, `chat_hook_plugin_abi.hpp`)
- [x] 검증 게이트 통과
  - `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`
  - `pwsh scripts/build.ps1 -Config Release`
  - `ctest --preset windows-test --output-on-failure`

1. `server/plugins/chat_hook_sample.cpp`를 `ChatHookApiV2` 기준으로 승격하고 v1 동작 호환을 유지한다.
2. `on_join` deny 시 `deny_reason`이 클라이언트 수신 payload로 전달되는 통합 테스트를 추가한다.
3. ABI/사용법 문서를 동기화한다.
   - `server/README.md`
   - `docs/core-api/extensions.md`
   - `server/include/server/chat/chat_hook_plugin_abi.hpp` Doxygen
4. 검증 게이트:
   - `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`
   - `pwsh scripts/build.ps1 -Config Release`
   - `ctest --preset windows-test --output-on-failure`

#### Stream B - Lua 기반 인프라 (P1)

1. 의존성/빌드 토글 정리:
   - LuaJIT(or Lua 5.4) + Sol2 도입
   - `BUILD_LUA_SCRIPTING` 옵션 추가
   - OFF 모드에서 Lua 코드 완전 비활성화 보장
2. `core/scripting` 구현:
   - `lua_runtime.hpp/.cpp`
   - `lua_sandbox.hpp/.cpp`
   - instruction/memory limit 및 forbidden libs 차단
3. core 레벨 테스트 우선 작성:
   - runtime 호출/오류/메모리 제한
   - sandbox 제한/타임아웃

#### Stream C - 서버 통합 및 hot-reload (P2)

1. `server/src/scripting/chat_lua_bindings.{hpp,cpp}` 추가 및 read-only/action API 구분 적용.
2. cold path hook에서만 Lua 호출 체인을 연결하고, hot path(`on_chat_send`, `on_pre_dispatch`)는 구조적으로 차단한다.
3. `ScriptWatcher` 연동으로 Lua 스크립트 hot-reload를 도입한다.
4. bootstrap wiring:
   - `server/src/app/bootstrap.cpp` 초기화/종료
   - `ServiceRegistry` 등록

#### Stream D - 안정성/관측성/운영화 (P2~P3)

1. 실패 격리:
   - 연속 실패 auto-disable
   - reload 시 자동 재활성화
2. 메트릭:
   - plugin/lua/common metrics 추가
   - `/metrics` 렌더링 및 Grafana 패널 반영
3. Docker/운영:
   - `docker/stack/docker-compose.yml` mount/env 정리
   - 샘플 스크립트/플러그인 운영 시나리오 검증
4. CI:
   - `BUILD_LUA_SCRIPTING=OFF/ON` 경로 분리 검증
   - 성능 회귀 게이트(특히 hot path) 추가

### 0.3 작업 단위 운영 규칙

- 각 스트림은 "코드 + 테스트 + 문서"를 한 세트로 완료하고 다음 스트림으로 진행한다.
- 모든 PR은 최소 아래 3개 증거를 포함한다.
  1. 변경 파일 `lsp_diagnostics` 에러 0
  2. 관련 테스트 실행 로그
  3. `ctest --preset windows-test` 결과(또는 예외 사유)
- hot path 성능에 영향이 있는 변경은 Lua 경로가 비활성화된 baseline과 비교해 수치로 보고한다.

---

## 1. 개요

### 1.1 현재 상태

서버 스택은 실험적 chat hook plugin 시스템을 보유하고 있다.

| 항목 | 현재 |
|---|---|
| 범위 | `MSG_CHAT_SEND` 핸들러 **단일 hook** |
| ABI | C ABI v1 (`ChatHookApiV1` 함수 테이블) |
| 로딩 | dlopen/LoadLibrary + cache-copy + mtime 폴링 |
| 안전장치 | lock/sentinel 파일, 예외 흡수, 고정 크기 버퍼 |
| 한계 | 다른 이벤트(login/join/leave/admin 등) 후킹 불가, 서버 상태 접근 불가, 플러그인 작성에 C/C++ 빌드 환경 필수 |

### 1.2 목표

1. **범용 hook**: `MSG_CHAT_SEND` 외에도 login, join, leave, session lifecycle, admin command, 스케줄 이벤트 등 다양한 지점에서 동적 로직을 주입할 수 있다.
2. **하이브리드 확장**: 성능 크리티컬 경로(hot path)는 네이티브 플러그인으로, 정책/커스텀 로직(cold path)은 Lua 스크립트로 처리한다.
3. **재사용 가능한 인프라**: 로딩/리로드/샌드박싱 메커니즘은 `core/`에 범용 프레임워크로 제공하여, gateway/wb_worker 등 다른 서비스에서도 동일하게 사용할 수 있다.
4. **안전성**: 스크립트 오류가 서버를 크래시하지 않고, 무한 루프/메모리 폭주를 방지하며, 실패 시 자동 비활성화한다.
5. **운영 친화**: 서버 재시작 없이 hot-reload, Docker 볼륨 마운트와 자연스럽게 통합, Prometheus 메트릭으로 관측 가능.

### 1.3 기대 효과

| 영역 | 기대 효과 |
|---|---|
| **운영** | 채팅 필터/금칙어/이벤트 알림 등을 서버 재배포 없이 즉시 변경 |
| **개발 속도** | Lua 스크립트로 프로토타입 → 검증 후 필요 시 네이티브로 승격 |
| **확장성** | gateway 라우팅 정책, wb_worker 이벤트 변환 등 동일 프레임워크로 확장 |
| **안정성** | 샌드박싱된 Lua 환경에서 실행 제한 + 자동 비활성화로 장애 격리 |
| **팀 효율** | 시스템 개발자는 네이티브 플러그인, 기획/운영자는 Lua 스크립트로 역할 분담 |

### 1.4 비목표

- 프로토콜 레벨 재설계.
- 기존 chat hook plugin v1 ABI의 즉시 폐기 (하위 호환 유지).
- 외부 프로세스 격리(out-of-process plugin) — 복잡도 대비 이점 부족.
- 사용자(클라이언트)가 직접 스크립트를 업로드하는 UGC 시나리오.

---

## 2. 아키텍처: 메커니즘-정책 분리

### 2.1 설계 원칙

기존 프로젝트 원칙을 그대로 따른다:

| 원칙 | 적용 |
|---|---|
| 의존 방향 단방향: `server/` → `core/` | core는 채팅/방/유저 개념을 모른다 |
| core는 범용 재사용 라이브러리 | 로딩/리로드/샌드박싱 메커니즘만 제공 |
| 역할 기반 네이밍 | 프로젝트명 금지, 기능 중심 이름 사용 |
| MSA 전환 대비 | gateway, wb_worker 등에서도 동일 프레임워크 사용 가능 |

### 2.2 계층 구조

```
┌──────────────────────────────────────────────────────────────────────┐
│                     서비스 계층 (정책)                                │
│                                                                      │
│  server/                  gateway/               tools/wb_worker/    │
│  ├─ ChatHookABI (v2)      ├─ GatewayFilterABI     ├─ WbTransformABI  │
│  ├─ ChatPluginChain       ├─ GatewayFilterChain   ├─ WbTransformChain│
│  ├─ ChatLuaBindings       ├─ GatewayLuaBindings   └─ (미래)          │
│  └─ Hook 호출 위치        └─ Hook 호출 위치                          │
│       (handlers_*.cpp)         (gateway_app.cpp)                     │
│                                                                      │
│  ─────────────────── 의존 방향: ↓ (단방향) ───────────────────────── │
│                                                                      │
│                     코어 계층 (메커니즘)                               │
│                                                                      │
│  core/include/server/core/                                           │
│  ├─ plugin/                                                          │
│  │   ├─ shared_library.hpp      # dlopen/LoadLibrary RAII wrapper    │
│  │   ├─ plugin_host.hpp         # 제네릭 로더 + hot-reload           │
│  │   └─ plugin_host_metrics.hpp # reload 카운터 스냅샷               │
│  └─ scripting/                                                       │
│      ├─ lua_runtime.hpp         # Sol2 state 관리                    │
│      ├─ lua_sandbox.hpp         # 환경 화이트리스트 + 실행 제한       │
│      └─ script_watcher.hpp      # 파일 감시 + atomic swap            │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.3 기존 패턴과의 일관성

이 분리는 프로젝트에 이미 존재하는 메커니즘-정책 패턴과 동일하다:

| core/ (메커니즘) | server/ (정책) |
|---|---|
| `Dispatcher` (범용 opcode 라우팅) | `register_routes()` (구체적 opcode→handler 매핑) |
| `SessionListener` (범용 TCP 수신) | bootstrap (구체적 설정, 포트, 옵션) |
| `JobQueue` (범용 작업 큐) | `ChatService` (도메인 작업 투입) |
| `MetricsHttpServer` (범용 HTTP) | `MetricsServer` (서버 전용 메트릭 렌더링) |
| **`PluginHost<T>`** (범용 로더) | **`ChatPluginChain`** (도메인 체인 로직) |
| **`LuaRuntime`** (범용 VM) | **Chat Lua 바인딩** (도메인 전용 API) |

---

## 3. Hook 분류: Hot Path vs Cold Path

런타임 확장의 성능 영향을 관리하기 위해, 모든 hook point를 빈도와 지연 민감도로 분류한다.

### 3.1 분류 기준

| 분류 | 빈도 | 지연 예산 | 권장 확장 방식 |
|---|---|---|---|
| **Hot** | 초당 수천~수만 회 | < 10μs | 네이티브 플러그인만 |
| **Warm** | 초당 수십~수백 회 | < 1ms | 네이티브 또는 Lua |
| **Cold** | 초당 수 회 이하 | < 10ms | Lua 권장 |

### 3.2 Hook Point 맵

| Hook Point | 위치 | 분류 | 입력 | 가능한 결정 |
|---|---|---|---|---|
| `on_chat_send` | `handlers_chat.cpp` | 🔴 Hot | session_id, room, user, text | pass, replace_text, block, handled |
| `on_whisper` | `handlers_chat.cpp` | 🟡 Warm | sender, target, text | pass, replace_text, block |
| `on_pre_dispatch` | `Dispatcher::dispatch()` | 🔴 Hot | session_id, opcode, payload_size | pass, drop |
| `on_login` | `handlers_login.cpp` | 🟢 Cold | session_id, nickname, credentials | allow, deny(reason) |
| `on_join` | `handlers_join.cpp` | 🟢 Cold | session_id, user, room, password | allow, deny(reason), redirect(room) |
| `on_leave` | `handlers_leave.cpp` | 🟢 Cold | session_id, user, room | pass (관측 전용) |
| `on_session_open` | `SessionListener` | 🟢 Cold | session_id, remote_addr | allow, deny |
| `on_session_close` | `Session::on_close` | 🟢 Cold | session_id, user, reason | pass (관측 전용) |
| `on_room_create` | `ChatService` | 🟢 Cold | room, owner | allow, deny, modify_options |
| `on_room_destroy` | `ChatService` | 🟢 Cold | room, reason | pass (관측 전용) |
| `on_admin_command` | admin fanout | 🟢 Cold | command, args, source | handled, pass |
| `on_timer` | `TaskScheduler` | 🟢 Cold | timer_name, tick_count | (사용자 정의 로직) |

### 3.3 결정(Decision) 체계

모든 hook은 통일된 결정 체계를 사용한다:

```
┌─────────────────────────────────────────────────────┐
│                    HookDecision                      │
├─────────────────────────────────────────────────────┤
│ kPass       │ 다음 플러그인/스크립트로 진행           │
│ kHandled    │ 기본 경로 중단, 응답은 hook이 처리      │
│ kBlock      │ 기본 경로 중단, 거부 응답 자동 생성     │
│ kModify     │ 입력을 변경하고 다음으로 진행            │
│ kAllow      │ 명시적 허용 (인증/접근 제어 hook용)      │
│ kDeny       │ 명시적 거부 + 사유 코드                  │
├─────────────────────────────────────────────────────┤
│ 각 hook은 자신에게 유효한 결정 부분집합만 허용한다.  │
│ 예: on_leave는 kPass만 허용 (관측 전용 hook).       │
└─────────────────────────────────────────────────────┘
```

---

## 4. Phase 1: 코어 플러그인 인프라 (`core/`)

> 예상 노력: 2~3일
> 안정성: `[Transitional]` → 검증 후 `[Stable]` 승격 예정

### 4.1 SharedLibrary

기존 `ChatHookPluginManager` 내부의 `SharedLibrary` 클래스를 `core/`로 추출한다.

**파일**: `core/include/server/core/plugin/shared_library.hpp`, `core/src/plugin/shared_library.cpp`

```cpp
namespace server::core::plugin {

/// @brief dlopen/LoadLibrary RAII wrapper.
///
/// 플랫폼별 동적 라이브러리 로딩을 추상화한다.
/// 소멸 시 자동으로 라이브러리를 언로드한다.
class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary();

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    SharedLibrary(SharedLibrary&&) noexcept;
    SharedLibrary& operator=(SharedLibrary&&) noexcept;

    /// @brief 지정 경로의 동적 라이브러리를 로드한다.
    /// @param path 라이브러리 파일 경로
    /// @param error 실패 시 오류 메시지 출력
    /// @return 성공 여부
    bool open(const std::filesystem::path& path, std::string& error);

    /// @brief 라이브러리를 명시적으로 언로드한다.
    void close();

    /// @brief 심볼을 조회한다.
    /// @param name 심볼 이름
    /// @param error 실패 시 오류 메시지 출력
    /// @return 심볼 주소 (실패 시 nullptr)
    void* symbol(const char* name, std::string& error) const;

    /// @brief 라이브러리가 로드되어 있는지 반환한다.
    bool is_loaded() const;
};

} // namespace server::core::plugin
```

### 4.2 PluginHost\<ApiTable\>

범용 플러그인 로더. 현재 `ChatHookPluginManager`의 로딩/리로드 로직을 제네릭 템플릿으로 추출한다.

**파일**: `core/include/server/core/plugin/plugin_host.hpp`

```cpp
namespace server::core::plugin {

/// @brief 범용 플러그인 호스트.
///
/// ApiTable은 C ABI 함수 테이블 구조체이다.
/// 서비스별로 자신만의 ApiTable을 정의하면
/// cache-copy, mtime 기반 hot-reload, sentinel 보호를 공통으로 사용할 수 있다.
///
/// read path는 atomic<shared_ptr>로 lock-free이며,
/// reload path만 mutex로 보호된다.
template <typename ApiTable>
class PluginHost {
public:
    struct Config {
        /// @brief 운영자가 배포/교체하는 원본 플러그인 경로
        std::filesystem::path plugin_path;

        /// @brief 원본을 복사해 안전하게 로드할 캐시 디렉터리
        std::filesystem::path cache_dir;

        /// @brief lock 파일이 존재하면 reload를 보류할 sentinel 경로 (옵션)
        std::optional<std::filesystem::path> lock_path;

        /// @brief 엔트리포인트 심볼 이름 (예: "chat_hook_api_v1")
        std::string entrypoint_symbol;

        /// @brief ABI 검증 콜백 — ApiTable 로드 후 버전/필수 필드를 확인한다.
        ///        false 반환 시 로드를 거부한다.
        std::function<bool(const ApiTable&)> validator;
    };

    /// @brief 로드된 플러그인 핸들. API 테이블과 인스턴스를 보유한다.
    struct LoadedPlugin {
        SharedLibrary lib;
        const ApiTable* api = nullptr;
        void* instance = nullptr;             // create()가 반환한 인스턴스
        std::filesystem::path cached_path;

        using DestroyFn = void(*)(void*);
        DestroyFn destroy_fn = nullptr;       // api->destroy (있을 경우)
    };

    explicit PluginHost(Config cfg);
    ~PluginHost();

    /// @brief mtime 변경 시 hot-reload를 수행한다.
    void poll_reload();

    /// @brief 현재 로드된 플러그인을 반환한다 (lock-free).
    std::shared_ptr<const LoadedPlugin> current() const;

    /// @brief reload 카운터 스냅샷
    struct MetricsSnapshot {
        std::filesystem::path plugin_path;
        bool loaded = false;
        std::uint64_t reload_attempt_total = 0;
        std::uint64_t reload_success_total = 0;
        std::uint64_t reload_failure_total = 0;
    };

    MetricsSnapshot metrics_snapshot() const;
};

} // namespace server::core::plugin
```

**핵심 동작 (현재 `ChatHookPluginManager::poll_reload()`와 동일):**
1. lock/sentinel 파일 존재 시 reload 보류
2. 원본 파일의 mtime 비교 — 변경 없으면 스킵
3. cache-copy: 원본을 캐시 디렉터리에 고유 이름으로 복사
4. 복사본을 dlopen/LoadLibrary로 로드
5. 엔트리포인트 심볼 조회 → ApiTable 포인터 획득
6. validator 콜백으로 ABI 검증
7. `atomic<shared_ptr>` store로 원자적 교체

### 4.3 PluginChainHost\<ApiTable\>

다중 플러그인 관리. 현재 `ChatHookPluginChain`의 디렉터리 스캔/순서 관리 로직을 추출한다.

**파일**: `core/include/server/core/plugin/plugin_chain_host.hpp`

```cpp
namespace server::core::plugin {

/// @brief 여러 PluginHost를 체인으로 관리한다.
///
/// 디렉터리 스캔 또는 명시적 경로 목록으로 플러그인을 발견하고,
/// 파일명 순서(또는 입력 순서)로 체인을 구성한다.
/// 체인 내 각 플러그인은 독립적으로 hot-reload된다.
template <typename ApiTable>
class PluginChainHost {
public:
    struct Config {
        /// @brief 명시 플러그인 경로 목록 (입력 순서 유지)
        std::vector<std::filesystem::path> plugin_paths;

        /// @brief plugin_paths가 비어 있을 때 사용할 디렉터리 모드 경로
        std::optional<std::filesystem::path> plugins_dir;

        /// @brief 모든 플러그인이 공유하는 cache-copy 디렉터리
        std::filesystem::path cache_dir;

        /// @brief 엔트리포인트 심볼 이름
        std::string entrypoint_symbol;

        /// @brief ABI 검증 콜백
        std::function<bool(const ApiTable&)> validator;
    };

    explicit PluginChainHost(Config cfg);

    /// @brief 디렉터리 재스캔 + 각 플러그인 poll_reload
    void poll_reload();

    /// @brief 현재 체인의 스냅샷 (순서 보장, lock-free read)
    using Chain = std::vector<std::shared_ptr<PluginHost<ApiTable>>>;
    std::shared_ptr<const Chain> current_chain() const;
};

} // namespace server::core::plugin
```

### 4.4 ScriptWatcher

Lua 스크립트 파일의 변경을 감지하고 atomic swap을 수행하는 범용 유틸리티.

**파일**: `core/include/server/core/scripting/script_watcher.hpp`

```cpp
namespace server::core::scripting {

/// @brief 스크립트 파일/디렉터리의 변경을 감지한다.
///
/// mtime 기반 폴링 + sentinel 파일을 지원한다.
/// 변경 감지 시 콜백을 호출하며, 콜백 실행은 호출자의 스레드에서 수행된다.
class ScriptWatcher {
public:
    struct Config {
        /// @brief 감시 대상 디렉터리
        std::filesystem::path scripts_dir;

        /// @brief 감시 대상 확장자 (예: ".lua")
        std::string extension = ".lua";

        /// @brief sentinel/lock 파일 경로 (옵션)
        std::optional<std::filesystem::path> lock_path;
    };

    /// @brief 변경된 파일 목록과 함께 호출되는 콜백
    using OnChangeCallback = std::function<void(const std::vector<std::filesystem::path>&)>;

    explicit ScriptWatcher(Config cfg, OnChangeCallback on_change);

    /// @brief 변경 여부를 확인하고 필요 시 콜백을 호출한다.
    void poll();
};

} // namespace server::core::scripting
```

---

## 5. Phase 2: Lua 런타임 인프라 (`core/`)

> 예상 노력: 3~4일
> 안정성: `[Transitional]`
> 의존성: Sol2 (sol3) + LuaJIT 2.1 (또는 PUC Lua 5.4)

### 5.1 기술 선택

| 항목 | 선택 | 근거 |
|---|---|---|
| 바인딩 | Sol2 (sol3) | C++20 네이티브 지원, zero-overhead 설계, `sol::protected_function`으로 에러 격리 |
| 엔진 | LuaJIT 2.1 (1순위), PUC Lua 5.4 (대안) | LuaJIT: ~7x 성능, FFI. PUC: 5.4 기능(`<const>`, `<close>`) + ARM64 안정성 |
| 빌드 통합 | CMake `option(BUILD_LUA_SCRIPTING)` | 기본 배포는 ON(기능 포함), OFF는 호환성/최소 빌드 용도 |

### 5.2 LuaRuntime

**파일**: `core/include/server/core/scripting/lua_runtime.hpp`, `core/src/scripting/lua_runtime.cpp`

```cpp
namespace server::core::scripting {

/// @brief Lua 실행 환경 관리자.
///
/// Sol2 state를 소유하고, 샌드박싱된 환경에서 스크립트를 실행한다.
/// 단일 스레드에서만 접근해야 한다 (strand/전용 스레드로 보호).
class LuaRuntime {
public:
    struct Config {
        /// @brief instruction 제한 (0 = 무제한)
        std::uint64_t instruction_limit = 100'000;

        /// @brief 메모리 제한 바이트 (0 = 무제한)
        std::size_t memory_limit_bytes = 1 * 1024 * 1024;  // 1 MB

        /// @brief 허용할 Lua 표준 라이브러리
        /// base, string, table, math, utf8만 기본 허용.
        /// os, io, debug, package, ffi는 기본 차단.
        std::vector<std::string> allowed_libs = {
            "base", "string", "table", "math", "utf8"
        };
    };

    explicit LuaRuntime(Config cfg);
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    /// @brief 스크립트 파일을 로드하고 환경에 등록한다.
    /// @param path 스크립트 파일 경로
    /// @param env_name 환경 이름 (격리된 샌드박스 네임스페이스)
    /// @return 성공 여부 + 실패 시 오류 메시지
    struct LoadResult { bool ok; std::string error; };
    LoadResult load_script(const std::filesystem::path& path,
                           const std::string& env_name);

    /// @brief 특정 환경의 함수를 호출한다.
    /// @param env_name 환경 이름
    /// @param func_name 함수 이름
    /// @param args 인자 (Sol2 호환 타입)
    /// @return 호출 결과 (sol::protected_function_result)
    ///
    /// 함수가 존재하지 않으면 호출을 건너뛴다 (capability discovery).
    template <typename... Args>
    CallResult call(const std::string& env_name,
                    const std::string& func_name,
                    Args&&... args);

    /// @brief 호스트 API 함수를 Lua 글로벌 테이블에 등록한다.
    /// @param table_name 테이블 이름 (예: "server")
    /// @param func_name 함수 이름 (예: "broadcast")
    /// @param func C++ 콜백
    template <typename Fn>
    void register_host_api(const std::string& table_name,
                           const std::string& func_name,
                           Fn&& func);

    /// @brief 모든 환경을 제거하고 state를 초기화한다.
    void reset();

    /// @brief 메트릭 스냅샷
    struct MetricsSnapshot {
        std::size_t loaded_scripts = 0;
        std::size_t memory_used_bytes = 0;
        std::uint64_t calls_total = 0;
        std::uint64_t errors_total = 0;
        std::uint64_t instruction_limit_hits = 0;
        std::uint64_t memory_limit_hits = 0;
    };

    MetricsSnapshot metrics_snapshot() const;
};

} // namespace server::core::scripting
```

### 5.3 LuaSandbox

**파일**: `core/include/server/core/scripting/lua_sandbox.hpp`

샌드박싱 정책을 분리한 유틸리티.

```cpp
namespace server::core::scripting {

/// @brief Lua 환경에 화이트리스트 기반 샌드박싱을 적용한다.
///
/// 블랙리스트(금지) 대신 화이트리스트(허용만 등록) 방식을 사용한다.
/// os, io, debug, package, ffi 등 위험한 라이브러리는 기본적으로 접근 불가하다.
namespace sandbox {

    /// @brief 안전한 기본 환경을 생성한다.
    /// @param state Sol2 state
    /// @param allowed_libs 허용할 라이브러리 이름 목록
    /// @return 격리된 sol::environment
    sol::environment create_safe_environment(
        sol::state& state,
        const std::vector<std::string>& allowed_libs);

    /// @brief instruction 제한 hook을 설치한다.
    /// @param state Sol2 state
    /// @param limit 최대 instruction 수
    void install_instruction_limit(sol::state& state, std::uint64_t limit);

    /// @brief 커스텀 메모리 할당자를 설치한다.
    /// @param state Sol2 state
    /// @param max_bytes 메모리 상한
    void install_memory_limit(sol::state& state, std::size_t max_bytes);

} // namespace sandbox
} // namespace server::core::scripting
```

### 5.4 스레드 모델

lua_State는 스레드 안전하지 않다. 아래 모델을 적용한다:

```
┌─────────────────────────────────────────────────────────────┐
│                    io_context (N 워커 스레드)                 │
│                                                              │
│  Thread 1 ─dispatch─▶ handler ─▶ 네이티브 플러그인 (직접)    │
│  Thread 2 ─dispatch─▶ handler ─▶ 네이티브 플러그인 (직접)    │
│  Thread N ─dispatch─▶ handler ─▶ 네이티브 플러그인 (직접)    │
│                                                              │
│  Cold path hook 발생 시:                                     │
│  Thread X ─post─▶ ┌──────────────────────────┐               │
│                    │   Lua Strand (직렬화)     │               │
│                    │   LuaRuntime::call(...)   │               │
│                    │   단일 lua_State          │               │
│                    └──────────────────────────┘               │
│                                                              │
│  Hot path (MSG_CHAT_SEND 등):                                │
│  Thread X ─▶ 네이티브 플러그인 체인 (lock-free)              │
│             Lua 호출 없음 (성능 보호)                        │
└─────────────────────────────────────────────────────────────┘
```

**규칙:**
- Hot path hook → 네이티브 플러그인만. Lua 호출 금지.
- Cold path hook → Lua strand로 post. 결과는 콜백/코루틴으로 수신.
- Lua strand가 병목이 되면 → State Pool 모델로 확장 (Phase 4).

---

## 6. Phase 3: 서버 측 통합 (`server/`)

> 예상 노력: 3~5일

### 6.1 네이티브 플러그인 ABI 확장 (v2)

기존 `ChatHookApiV1`을 확장하여 다중 hook point를 지원한다.

**파일**: `server/include/server/chat/chat_hook_plugin_abi.hpp` (확장)

```cpp
extern "C" {

static constexpr std::uint32_t CHAT_HOOK_ABI_VERSION_V2 = 2u;

/// @brief 범용 hook 결정값.
enum class HookDecisionV2 : std::uint32_t {
    kPass    = 0,   // 다음 플러그인으로 진행
    kHandled = 1,   // 기본 경로 중단, hook이 처리
    kBlock   = 2,   // 기본 경로 중단, 거부 응답
    kModify  = 3,   // 입력 변경 후 다음으로 진행
    kAllow   = 4,   // 명시적 허용
    kDeny    = 5,   // 명시적 거부 + 사유
};

/// @brief 공통 문자열 버퍼 (v1과 동일).
struct HookStrBufV2 {
    char* data;
    std::uint32_t capacity;
    std::uint32_t size;
};

// ── Login Hook ──
struct LoginEventV2 {
    std::uint32_t session_id;
    const char* nickname;
    const char* remote_addr;
};

struct LoginEventOutV2 {
    HookStrBufV2 deny_reason;
};

// ── Join Hook ──
struct JoinEventV2 {
    std::uint32_t session_id;
    const char* user;
    const char* room;
    bool has_password;
};

struct JoinEventOutV2 {
    HookStrBufV2 deny_reason;
    HookStrBufV2 redirect_room;
};

// ── Leave Hook ──
struct LeaveEventV2 {
    std::uint32_t session_id;
    const char* user;
    const char* room;
};

// ── Session Lifecycle Hook ──
struct SessionEventV2 {
    std::uint32_t session_id;
    const char* remote_addr;
    const char* user;           // nullptr if not yet authenticated
    std::uint32_t event_type;   // 0=open, 1=close
};

// ── Admin Command Hook ──
struct AdminCommandV2 {
    const char* command;
    const char* args;
    const char* source;
};

struct AdminCommandOutV2 {
    HookStrBufV2 response;
};

/// @brief 확장된 플러그인 API 함수 테이블 (v2).
///
/// nullptr인 함수 포인터는 해당 hook을 구현하지 않음을 의미한다.
/// 이로써 기존 v1 플러그인도 on_chat_send만 구현하고 나머지는 nullptr로 둘 수 있다.
struct ChatHookApiV2 {
    std::uint32_t abi_version;          // = CHAT_HOOK_ABI_VERSION_V2
    const char* name;
    const char* version;

    // 인스턴스 수명주기
    void* (*create)();
    void  (*destroy)(void* instance);

    // ── Hot path hooks ──
    ChatHookDecisionV1 (*on_chat_send)(
        void* instance,
        const ChatHookChatSendV1* in,
        ChatHookChatSendOutV1* out);

    // ── Cold path hooks (모두 선택적 — nullptr 허용) ──
    HookDecisionV2 (*on_login)(void* instance, const LoginEventV2* in, LoginEventOutV2* out);
    HookDecisionV2 (*on_join)(void* instance, const JoinEventV2* in, JoinEventOutV2* out);
    HookDecisionV2 (*on_leave)(void* instance, const LeaveEventV2* in);
    HookDecisionV2 (*on_session_event)(void* instance, const SessionEventV2* in);
    HookDecisionV2 (*on_admin_command)(void* instance, const AdminCommandV2* in, AdminCommandOutV2* out);
};

/// @brief v2 엔트리포인트.
CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2();

} // extern "C"
```

**하위 호환성:**
- v1 플러그인은 `chat_hook_api_v1()` 엔트리포인트를 유지한다.
- 로더는 먼저 `chat_hook_api_v2()`를 시도하고, 없으면 `chat_hook_api_v1()`으로 폴백한다.
- v1 API 테이블은 `on_chat_send`만 추출하여 v2 wrapper로 감싼다.

### 6.2 서버 ChatPluginChain 리팩터링

`ChatHookPluginChain`을 `core::plugin::PluginChainHost<ChatHookApiV2>`를 기반으로 리팩터링한다.

```cpp
// server/src/chat/chat_plugin_chain.hpp (리팩터링)
namespace server::app::chat {

class ChatPluginChain {
public:
    // core의 범용 체인 호스트를 내부에서 사용
    using Host = server::core::plugin::PluginChainHost<ChatHookApiV2>;

    // 도메인 특화 호출 메서드
    Outcome on_chat_send(uint32_t session_id, std::string_view room,
                         std::string_view user, std::string& text) const;

    HookDecisionV2 on_login(uint32_t session_id, std::string_view nickname,
                            std::string_view remote_addr,
                            std::string& deny_reason) const;

    HookDecisionV2 on_join(uint32_t session_id, std::string_view user,
                           std::string_view room, bool has_password,
                           std::string& deny_reason,
                           std::string& redirect_room) const;
    // ... 기타 hook 메서드
};

} // namespace server::app::chat
```

### 6.3 Lua 서버 바인딩

서버 도메인 전용 Lua API를 정의한다. 이 바인딩은 `server/`에 위치한다 (core가 아님).

**파일**: `server/src/scripting/chat_lua_bindings.hpp`, `server/src/scripting/chat_lua_bindings.cpp`

```lua
-- Lua 스크립트에 노출되는 서버 API (화이트리스트)
-- 모든 함수는 논블로킹이다.

server = {
    -- ── 읽기 전용 조회 (ID 기반, 내부 포인터 노출 없음) ──
    get_user_name(session_id)       --> string | nil
    get_user_room(session_id)       --> string | nil
    get_room_users(room_name)       --> {string, string, ...}
    get_room_list()                 --> {string, string, ...}
    get_room_owner(room_name)       --> string | nil
    is_user_muted(nickname)         --> boolean
    is_user_banned(nickname)        --> boolean
    get_online_count()              --> number
    get_room_count()                --> number

    -- ── 액션 (JobQueue로 post, 즉시 반환) ──
    send_notice(session_id, text)              --> void
    broadcast_room(room_name, text)            --> void
    broadcast_all(text)                        --> void
    kick_user(session_id, reason)              --> void
    mute_user(nickname, duration_sec, reason)  --> void
    ban_user(nickname, duration_sec, reason)   --> void

    -- ── 로깅 ──
    log_info(msg)     --> void
    log_warn(msg)     --> void
    log_debug(msg)    --> void

    -- ── 메타 ──
    hook_name()       --> string   (현재 실행 중인 hook 이름)
    script_name()     --> string   (현재 스크립트 파일 이름)
}
```

**설계 원칙:**
- **ID 기반 접근**: session_id, room_name 등 문자열/숫자 식별자로만 엔티티를 참조한다. 내부 포인터/참조를 Lua에 노출하지 않는다.
- **스냅샷 반환**: `get_room_users()`는 호출 시점의 복사본을 반환한다. Lua가 보유한 테이블이 서버 상태와 동기화되지 않는다.
- **논블로킹 액션**: `kick_user()` 등은 JobQueue에 작업을 post하고 즉시 반환한다. Lua 스크립트가 io_context 스레드를 블로킹하지 않는다.
- **제한된 표면**: 최소한의 API만 노출하고, 필요 시 점진적으로 확장한다.

### 6.4 Hook 호출 통합

각 핸들러에서 네이티브 플러그인과 Lua 스크립트를 모두 호출하는 통합 흐름:

```
Handler 진입 (예: on_login)
    │
    ▼
┌───────────────────────────────┐
│ 1. 네이티브 플러그인 체인      │  ← 동기 호출 (현재 스레드)
│    ChatPluginChain::on_login() │
│    결과: kPass / kDeny / ...   │
└───────────┬───────────────────┘
            │ kPass인 경우
            ▼
┌───────────────────────────────┐
│ 2. Lua 스크립트 호출           │  ← Cold path: Lua strand로 post
│    LuaRuntime::call(           │     Hot path: 스킵
│      "on_login", ctx)          │
│    결과: kPass / kDeny / ...   │
└───────────┬───────────────────┘
            │ kPass인 경우
            ▼
┌───────────────────────────────┐
│ 3. 기본 핸들러 로직            │  ← 기존 코드 그대로
└───────────────────────────────┘
```

**우선순위**: 네이티브 플러그인 → Lua 스크립트 → 기본 로직. 앞 단계에서 kBlock/kDeny/kHandled가 반환되면 뒤 단계는 실행하지 않는다.

---

## 7. Phase 4: 안전장치와 관측성

> 예상 노력: 1~2일

### 7.1 안전장치

| 위험 | 방어 메커니즘 |
|---|---|
| **Lua 무한 루프** | `lua_sethook` — instruction count 제한 (기본 100,000). 초과 시 `LUA_ERRRUN` 발생 → 해당 호출만 실패 |
| **Lua 메모리 폭주** | 커스텀 allocator — `lua_newstate(alloc_fn)`. 상한 초과 시 할당 거부 → `LUA_ERRMEM` |
| **네이티브 플러그인 크래시** | try/catch로 예외 흡수 (현재와 동일). SEH/signal은 프로세스 레벨이므로 방어 불가 — 운영 가이드로 대응 |
| **Lua 스크립트 오류** | `sol::protected_function` — Lua 에러를 C++ 예외로 변환하지 않고 결과 객체로 반환 |
| **연속 실패** | 자동 비활성화: 동일 스크립트/플러그인이 N회(기본 3) 연속 실패 시 해당 hook을 비활성화하고 경고 로그 + 메트릭 기록 |
| **io_context 블로킹** | API 설계 수준에서 차단: Lua에 노출하는 모든 호스트 API는 논블로킹. 블로킹 가능한 API(DB 쿼리, HTTP 호출 등)를 노출하지 않는다 |

### 7.2 Prometheus 메트릭

| 메트릭 이름 | 타입 | 설명 |
|---|---|---|
| `plugin_reload_attempt_total` | Counter | 플러그인 reload 시도 횟수 (label: plugin_name) |
| `plugin_reload_success_total` | Counter | 플러그인 reload 성공 횟수 |
| `plugin_reload_failure_total` | Counter | 플러그인 reload 실패 횟수 |
| `plugin_hook_calls_total` | Counter | Hook 호출 횟수 (label: hook_name, plugin_name) |
| `plugin_hook_errors_total` | Counter | Hook 오류 횟수 |
| `plugin_hook_duration_seconds` | Histogram | Hook 실행 시간 |
| `lua_script_calls_total` | Counter | Lua 스크립트 호출 횟수 (label: hook_name, script_name) |
| `lua_script_errors_total` | Counter | Lua 스크립트 오류 횟수 |
| `lua_instruction_limit_hits_total` | Counter | Instruction 제한 도달 횟수 |
| `lua_memory_limit_hits_total` | Counter | 메모리 제한 도달 횟수 |
| `lua_memory_used_bytes` | Gauge | 현재 Lua 메모리 사용량 |
| `lua_loaded_scripts` | Gauge | 현재 로드된 스크립트 수 |
| `hook_auto_disable_total` | Counter | 연속 실패로 인한 자동 비활성화 횟수 |

---

## 8. 사용 시나리오별 가이드

### 8.1 네이티브 플러그인이 적합한 경우

네이티브 플러그인은 **성능이 최우선**이거나 **저수준 제어**가 필요할 때 사용한다.

#### 예시 1: 고성능 채팅 필터 (Hot path)

초당 수만 건의 채팅 메시지를 처리하면서 금칙어/스팸을 필터링해야 한다. 정규식 매칭, Aho-Corasick 알고리즘 등 CPU 집약적 로직이 필요하다.

```cpp
// plugins/30_spam_filter.cpp
extern "C" {

static ChatHookDecisionV1 CHAT_HOOK_CALL on_chat_send(
    void* instance,
    const ChatHookChatSendV1* in,
    ChatHookChatSendOutV1* out)
{
    auto* filter = static_cast<SpamFilter*>(instance);

    // Aho-Corasick 기반 금칙어 매칭 (~100ns)
    if (filter->contains_banned_word(in->text)) {
        write_buf(&out->notice, "금칙어가 포함된 메시지입니다.");
        return ChatHookDecisionV1::kBlock;
    }

    // 스팸 레이트 리밋 (~50ns)
    if (filter->is_rate_limited(in->session_id)) {
        write_buf(&out->notice, "메시지를 너무 빠르게 보내고 있습니다.");
        return ChatHookDecisionV1::kBlock;
    }

    return ChatHookDecisionV1::kPass;
}

} // extern "C"
```

**왜 네이티브인가**: 매 채팅 메시지마다 실행되는 hot path에서 Lua VM 오버헤드(~200ns)는 누적 시 무시할 수 없다. 네이티브 함수 포인터 호출(~2ns)은 두 자릿수 이상 빠르다.

#### 예시 2: 패킷 검증/변환 (Hot path)

모든 수신 패킷에 대해 pre-dispatch 검증을 수행한다.

```cpp
// plugins/01_packet_validator.cpp
extern "C" {

// v2 ABI — on_pre_dispatch는 모든 패킷마다 호출됨
static HookDecisionV2 CHAT_HOOK_CALL on_pre_dispatch(
    void* instance,
    std::uint32_t session_id,
    std::uint16_t opcode,
    std::uint32_t payload_size)
{
    // 비정상적으로 큰 패킷 차단
    if (payload_size > MAX_EXPECTED_SIZE) {
        return HookDecisionV2::kBlock;
    }
    return HookDecisionV2::kPass;
}

} // extern "C"
```

#### 예시 3: 커스텀 암호화/압축 계층

네트워크 계층에 가까운 저수준 변환은 네이티브만 가능하다.

### 8.2 Lua 스크립트가 적합한 경우

Lua는 **빠른 이터레이션**, **운영 중 즉시 변경**, **비개발자도 수정 가능한 로직**에 적합하다.

#### 예시 1: 로그인 환영 메시지 + 이벤트 알림 (Cold path)

```lua
-- scripts/on_login_welcome.lua
-- 로그인 시 환영 메시지와 현재 진행 중인 이벤트를 알린다.
-- 이벤트 내용을 바꾸려면 이 파일만 수정하면 된다 (서버 재시작 불필요).

function on_login(ctx)
    local name = server.get_user_name(ctx.session_id)
    if not name then
        return { decision = "pass" }
    end

    -- 환영 메시지
    server.send_notice(ctx.session_id,
        name .. "님, 환영합니다! 현재 " ..
        server.get_online_count() .. "명이 접속 중입니다.")

    -- 이벤트 알림 (운영자가 수시로 변경)
    server.send_notice(ctx.session_id,
        "[이벤트] 2026년 3월 한정! 봄맞이 채팅 이벤트 진행 중!")

    return { decision = "pass" }
end
```

**왜 Lua인가**: 이벤트 문구는 수시로 변경된다. C++ 플러그인으로 만들면 변경할 때마다 빌드→배포→리로드가 필요하지만, Lua 파일은 텍스트 에디터로 수정하고 저장하면 자동으로 반영된다.

#### 예시 2: 방 입장 제어 정책 (Cold path)

```lua
-- scripts/on_join_policy.lua
-- 특정 방에 대한 접근 제어 정책.
-- 비개발자(운영자)가 직접 정책을 수정할 수 있다.

-- VIP 방 목록 (운영자가 수시로 변경)
local vip_rooms = {
    ["vip-lounge"] = true,
    ["staff-room"] = true,
}

-- VIP 유저 목록
local vip_users = {
    ["admin"] = true,
    ["moderator01"] = true,
    ["moderator02"] = true,
}

function on_join(ctx)
    -- VIP 방이 아니면 통과
    if not vip_rooms[ctx.room] then
        return { decision = "pass" }
    end

    -- VIP 유저인지 확인
    if vip_users[ctx.user] then
        return { decision = "allow" }
    end

    -- 비VIP 유저 거부
    return {
        decision = "deny",
        reason = "이 방은 VIP 전용입니다."
    }
end
```

**왜 Lua인가**: VIP 목록이나 방 정책은 운영 중 자주 바뀐다. 빌드 없이 Lua 파일 수정만으로 즉시 반영할 수 있고, 롤백도 파일 복원으로 간단하다.

#### 예시 3: 자동 모더레이션 (Cold path)

```lua
-- scripts/on_chat_moderation.lua
-- Cold path용 채팅 모더레이션 (Lua strand에서 실행).
-- 복잡한 조건부 로직은 Lua가 더 표현력이 좋다.
-- 참고: 이 스크립트는 Cold path 전용이다. Hot path 채팅 필터는
--       네이티브 플러그인(예: 30_spam_filter.so)을 사용한다.

-- 경고 누적 횟수 (Lua 리로드 시 초기화됨)
local warnings = {}

function on_chat_moderation(ctx)
    local name = ctx.user

    -- 대문자만 사용하는 메시지 감지 (도배 의심)
    if #ctx.text > 10 and ctx.text == ctx.text:upper() then
        warnings[name] = (warnings[name] or 0) + 1

        if warnings[name] >= 3 then
            server.mute_user(name, 300, "반복적인 대문자 도배")
            server.send_notice(ctx.session_id,
                "대문자 도배로 5분간 채팅이 제한됩니다.")
            warnings[name] = 0
            return { decision = "block" }
        end

        server.send_notice(ctx.session_id,
            "경고 " .. warnings[name] .. "/3: 대문자만 사용하지 마세요.")
    end

    return { decision = "pass" }
end
```

#### 예시 4: 커스텀 관리자 명령어 (Cold path)

```lua
-- scripts/admin_commands.lua
-- 서버 재시작 없이 관리자 명령어를 추가/수정할 수 있다.

function on_admin_command(ctx)
    if ctx.command == "announce" then
        -- 전체 공지
        server.broadcast_all("[공지] " .. ctx.args)
        return { decision = "handled", response = "공지 전송 완료" }

    elseif ctx.command == "room_info" then
        -- 방 정보 조회
        local users = server.get_room_users(ctx.args)
        local info = ctx.args .. ": " .. #users .. "명"
        return { decision = "handled", response = info }

    elseif ctx.command == "clear_warnings" then
        -- 경고 초기화 (다른 스크립트의 상태는 접근 불가 — 격리)
        return { decision = "handled", response = "경고 초기화는 스크립트 리로드로 수행하세요." }
    end

    return { decision = "pass" }
end
```

#### 예시 5: 주기적 이벤트 / 스케줄 작업 (Cold path)

```lua
-- scripts/on_timer_hourly.lua
-- 매 시간마다 실행되는 정기 작업.

local last_hour = -1

function on_timer(ctx)
    local current_hour = os.date("*!%H")   -- UTC 시간 (os.date는 샌드박스에서 허용)
    if current_hour == last_hour then
        return
    end
    last_hour = current_hour

    -- 정시 알림
    local online = server.get_online_count()
    server.broadcast_all(
        "[시스템] 현재 시각 " .. current_hour .. ":00 UTC, " ..
        online .. "명 접속 중")

    -- 특정 시간대 이벤트
    if current_hour == "12" then
        server.broadcast_all("[이벤트] 점심시간 보너스 채팅 이벤트 시작!")
    end
end
```

### 8.3 하이브리드 조합 예시

하나의 hook point에서 네이티브와 Lua를 조합하는 사례:

```
MSG_CHAT_SEND 처리 흐름:
    │
    ▼
┌─────────────────────────────────────┐
│ 네이티브: 01_packet_validator.so    │  ← 패킷 크기/형식 검증 (~10ns)
│ 네이티브: 30_spam_filter.so         │  ← Aho-Corasick 금칙어 (~100ns)
│ 네이티브: 40_rate_limiter.so        │  ← 토큰 버킷 레이트리밋 (~50ns)
└─────────────┬───────────────────────┘
              │ kPass
              ▼
┌─────────────────────────────────────┐
│ 기본 채팅 로직 (브로드캐스트)        │  ← 기존 코드
└─────────────┬───────────────────────┘
              │ 전송 완료 후 (post-send, 비동기)
              ▼
┌─────────────────────────────────────┐
│ Lua strand: on_chat_after.lua       │  ← 채팅 통계, 업적, 이벤트 체크
│  (결과가 송신에 영향 없음)           │     (~1ms 허용)
└─────────────────────────────────────┘
```

---

## 9. 배포와 운영

### 9.1 디렉터리 구조

```
# Docker 컨테이너 내부
/app/
├─ plugins/                     # 네이티브 플러그인 (.so)
│  ├─ 01_packet_validator.so
│  ├─ 30_spam_filter.so
│  ├─ 40_rate_limiter.so
│  └─ staging/                  # 교체용 대기 바이너리
│     └─ 30_spam_filter_v2.so
├─ scripts/                     # Lua 스크립트
│  ├─ on_login_welcome.lua
│  ├─ on_join_policy.lua
│  ├─ on_chat_moderation.lua
│  ├─ admin_commands.lua
│  └─ on_timer_hourly.lua
└─ server_app                   # 서버 바이너리
```

### 9.2 환경 변수

```bash
# 네이티브 플러그인 (기존 변수 유지 + 확장)
CHAT_HOOK_PLUGINS_DIR=/app/plugins         # 디렉터리 모드 (파일명 순서)
CHAT_HOOK_PLUGIN_PATHS=                    # 또는 명시적 경로 목록
CHAT_HOOK_CACHE_DIR=/tmp/chat_hook_cache
CHAT_HOOK_RELOAD_INTERVAL_MS=500

# Lua 스크립팅 (신규)
LUA_SCRIPTS_DIR=/app/scripts               # 스크립트 디렉터리
LUA_RELOAD_INTERVAL_MS=1000                # 스크립트 리로드 폴링 주기
LUA_INSTRUCTION_LIMIT=100000               # instruction 제한
LUA_MEMORY_LIMIT_BYTES=1048576             # 메모리 제한 (1MB)
LUA_ENABLED=1                              # Lua 기능 활성화 (0=비활성)
LUA_AUTO_DISABLE_THRESHOLD=3               # 연속 N회 실패 시 자동 비활성화
```

### 9.3 Docker Compose 통합

```yaml
# docker/stack/docker-compose.yml (발췌)
services:
  server-1:
    image: server:local
    volumes:
      - ./plugins:/app/plugins:ro         # 네이티브 플러그인 마운트
      - ./scripts:/app/scripts:ro         # Lua 스크립트 마운트
    environment:
      - CHAT_HOOK_PLUGINS_DIR=/app/plugins
      - LUA_SCRIPTS_DIR=/app/scripts
      - LUA_ENABLED=1
```

### 9.4 Hot-Reload 운영 절차

#### 네이티브 플러그인 교체 (기존과 동일)

```bash
# 1. sentinel 파일로 reload 보류
docker exec server-1 touch /app/plugins/30_spam_filter_LOCK

# 2. 바이너리 교체
docker exec server-1 cp /app/plugins/staging/30_spam_filter_v2.so \
                        /app/plugins/30_spam_filter.so

# 3. sentinel 해제 → 자동 reload
docker exec server-1 rm -f /app/plugins/30_spam_filter_LOCK
```

#### Lua 스크립트 교체 (더 간단)

```bash
# 파일 수정/교체만으로 자동 반영 (sentinel 불필요)
docker exec server-1 cp /tmp/new_policy.lua /app/scripts/on_join_policy.lua

# 또는 호스트에서 직접 편집 (볼륨 마운트 시)
vim docker/stack/scripts/on_join_policy.lua
# 저장하면 다음 poll 주기(기본 1초)에 자동 반영
```

#### 롤백

```bash
# 네이티브: 이전 버전 파일로 교체
docker exec server-1 cp /app/plugins/staging/30_spam_filter_v1.so \
                        /app/plugins/30_spam_filter.so

# Lua: git revert 또는 파일 복원
git checkout HEAD~1 -- docker/stack/scripts/on_join_policy.lua
```

---

## 10. 마이그레이션 경로

### 10.1 기존 v1 플러그인 호환

| 상황 | 동작 |
|---|---|
| v1 플러그인 + v2 로더 | 로더가 `chat_hook_api_v2()` 실패 → `chat_hook_api_v1()` 폴백 → `on_chat_send`만 활성화 |
| v2 플러그인 + v2 로더 | 모든 hook 사용 가능 |
| v1/v2 혼합 체인 | 각 플러그인의 ABI 버전에 맞게 개별 호출 |

### 10.2 단계별 도입

```
Phase 1: core/ 인프라 추출 (PluginHost, SharedLibrary)
    → 기존 ChatHookPluginManager를 core::PluginHost 기반으로 리팩터링
    → 기존 동작 100% 유지 (행동 변경 없음)
    → 테스트: 기존 플러그인 hot-reload 시나리오 통과

Phase 2: ABI v2 확장 + 신규 hook point 추가
    → v1 하위 호환 유지
    → login/join/leave hook 추가
    → 테스트: v1 + v2 혼합 체인 동작 확인

Phase 3: Lua 런타임 추가 (선택적 기능)
    → BUILD_LUA_SCRIPTING=ON/OFF 옵션
    → Cold path hook에 Lua 통합
    → 테스트: 샌드박싱, instruction limit, 자동 비활성화

Phase 4: 관측성 + 안전장치 강화
    → Prometheus 메트릭 통합
    → 자동 비활성화, 시간 예산
    → 부하 테스트: hot path 지연 시간 회귀 없음 확인
```

---

## 11. 리스크와 완화 전략

| 리스크 | 영향 | 완화 |
|---|---|---|
| Lua GC가 hot path 지연에 영향 | p99 latency spike | Hot path에서 Lua 호출 금지 (아키텍처 수준 차단) |
| 네이티브 플러그인 메모리 오염 | 서버 크래시 | try/catch 흡수 + 자동 비활성화 + 운영 가이드 (신뢰할 수 있는 플러그인만 배포) |
| ABI v2 설계 실수 → 하위 호환 파괴 | 기존 플러그인 로드 실패 | v1 폴백 로직 + CI에서 v1 플러그인 로드 테스트 |
| Lua 바인딩 API 표면 확대 → 유지보수 부담 | 개발 속도 저하 | 최소 API 원칙: 필요가 검증된 API만 추가 |
| 스레드 안전성 버그 | 데이터 레이스, 크래시 | Lua는 strand 보호, 네이티브는 명시적 동시성 계약 문서화 |

---

## 12. 테스트 전략

| 테스트 유형 | 대상 | 방법 |
|---|---|---|
| **단위 테스트** | `PluginHost`, `LuaRuntime`, `SharedLibrary` | GTest, 모의 플러그인/스크립트 |
| **통합 테스트** | hook 호출 체인 (네이티브+Lua) | 테스트용 플러그인 + 스크립트로 end-to-end 검증 |
| **호환 테스트** | v1↔v2 혼합 로딩 | CI에서 v1 샘플 플러그인 로드 확인 |
| **샌드박스 테스트** | instruction limit, memory limit | 의도적으로 무한 루프/대량 할당 스크립트 실행 → 제한 동작 확인 |
| **부하 테스트** | hot path 지연 시간 회귀 | `MSG_CHAT_SEND` 초당 10,000건 → p99 latency 비교 (플러그인 유/무) |
| **자동 비활성화 테스트** | 연속 실패 → disable | 의도적 오류 스크립트 → N회 후 비활성화 확인 |
| **스모크 테스트** | Docker 풀스택 | `scripts/deploy_docker.ps1` → 플러그인+스크립트 로드 → 메시지 송수신 |

---

## 13. 관련 문서

| 문서 | 경로 |
|---|---|
| 기존 Chat Hook Plugin ABI | `server/include/server/chat/chat_hook_plugin_abi.hpp` |
| 확장 계약 거버넌스 | `docs/core-api/extensions.md` |
| 호환성 정책 | `docs/core-api/compatibility-policy.md` |
| Gateway 확장 인터페이스 후보 | `docs/core-api/gateway-extension-interface.md` |
| Write-behind 확장 인터페이스 후보 | `docs/core-api/write-behind-extension-interface.md` |
| 서버 아키텍처 | `docs/server-architecture.md` |
| 저장소 구조 가이드 | `docs/repo-structure.md` |
| 동시성 API 가이드 | `docs/core-api/concurrency.md` |
| 플러그인 초보자 가이드 (신규 예정) | `docs/extensibility/plugin-quickstart.md` |
| Lua 초보자 가이드 (신규 예정) | `docs/extensibility/lua-quickstart.md` |
| 충돌 정책 가이드 (신규 예정) | `docs/extensibility/conflict-policy.md` |
| 운영 레시피 모음 (신규 예정) | `docs/extensibility/recipes.md` |

---

## 14. 추가 운영 아이디어 평가 및 반영

사용자 제안 아이디어를 실현 가능성, 효용성, 구현 비용 기준으로 평가하고 본 계획에 반영한다.

### 14.1 아이디어 타당성 평가

| 아이디어 | 실현 가능성 | 효용성 | 구현 난이도 | 판단 |
|---|---|---|---|---|
| 관리 콘솔에서 대상 서버 지정 후 플러그인/스크립트 적용 | 높음 | 매우 높음 | 중간 | **즉시 채택** |
| 지정 경로 파일 자동 감지 + 선택 UI 제공 | 높음 | 높음 | 낮음~중간 | **즉시 채택** |
| 시각 예약 교체 (단일 서버/전체 서버 타겟) | 중간~높음 | 매우 높음 | 중간~높음 | **채택 (2단계 도입)** |
| 동일 hook 영역 충돌 방지 정책 | 높음 | 매우 높음 | 중간 | **필수 채택** |
| 템플릿 + 초보자 가이드 제공 | 매우 높음 | 높음 | 낮음 | **즉시 채택** |

결론:

- 제안된 아이디어 5건 모두 실현 가능하며, 운영 효율/안전성 개선 효과가 크다.
- 단, 예약 교체는 시계 동기화/멱등성/롤백 시나리오가 필요하므로 단계적으로 도입한다.

### 14.2 관리 콘솔 제어면(Control Plane) 설계

#### 14.2.1 목표

- 운영자가 UI에서 플러그인/스크립트를 선택하고, 대상 서버(개별/그룹/전체)에 즉시 또는 예약 적용할 수 있게 한다.
- MMORPG/멀티월드 시나리오를 위해 서버별로 다른 정책 조합을 적용할 수 있게 한다.

#### 14.2.2 아키텍처 개요

```
┌──────────────────────────────┐
│ Admin Console (UI/API)       │
│ - Inventory 조회              │
│ - Rollout 계획 생성           │
│ - 예약 작업 관리              │
└──────────────┬───────────────┘
               │ signed command
               ▼
┌──────────────────────────────┐
│ Control Plane Service         │
│ - Artifact Inventory Cache    │
│ - Rollout Scheduler           │
│ - Target Resolver             │
└──────────────┬───────────────┘
               │ Redis pub/sub (admin channel)
               ▼
┌─────────────────────────────────────────────────────────┐
│ server_app instances                                     │
│ - command signature/TTL 검증                            │
│ - target selector 매칭 확인                             │
│ - plugin/script apply + 결과 ack                        │
│ - metrics/log emit                                       │
└─────────────────────────────────────────────────────────┘
```

#### 14.2.3 타겟 선택 모델

타겟 선택자는 아래 조합을 지원한다:

| selector | 설명 | 예시 |
|---|---|---|
| `all` | 전체 서버 대상 | 운영 이벤트 일괄 적용 |
| `server_ids` | 특정 서버 ID 집합 | `server-chat-01`, `server-chat-07` |
| `roles` | 역할 기반 매칭 | `chat`, `world`, `field`, `lobby` |
| `regions` | 리전 기반 매칭 | `kr-seoul-a`, `ap-tokyo-c` |
| `shards` | 샤드 기반 매칭 | `shard-1`, `shard-2` |
| `tags` | 자유 태그 기반 매칭 | `event-enabled`, `vip-zone` |

제안 환경 변수/메타데이터:

- `SERVER_ROLE`
- `SERVER_REGION`
- `SERVER_SHARD`
- `SERVER_TAGS` (쉼표 구분)

### 14.3 지정 경로 파일 감지 + 선택 UI

#### 14.3.1 인벤토리 스캔

- `plugins/`와 `scripts/`를 주기적으로 스캔하여 관리 콘솔 인벤토리를 구성한다.
- 파일 목록만 노출하지 않고, 메타데이터(manifest)를 함께 읽어 충돌/호환 검증에 활용한다.

권장 manifest 예시:

```
/app/plugins/30_spam_filter.so
/app/plugins/30_spam_filter.plugin.json

/app/scripts/on_join_policy.lua
/app/scripts/on_join_policy.script.json
```

manifest 주요 필드:

| 필드 | 설명 |
|---|---|
| `name`, `version` | 표시용 이름/버전 |
| `kind` | `native_plugin` / `lua_script` |
| `hook_scope` | 적용 hook 목록 (`on_chat_send`, `on_join` 등) |
| `stage` | 실행 단계 (`pre_validate`, `mutate`, `authorize`, `observe`) |
| `priority` | 동일 stage 내 실행 우선순위 |
| `exclusive_group` | 상호 배타 그룹 이름 |
| `target_profiles` | 권장 대상(`chat`, `world`, `all`) |
| `checksum` | 무결성 검증용 sha256 |
| `compat` | 최소/최대 ABI, 필요 기능 |

### 14.4 예약 교체(스케줄 배포)

#### 14.4.1 기능 요구

- 특정 UTC 시각(`run_at_utc`)에 자동 적용.
- 단일 서버/서버 그룹/전체 서버 대상 선택.
- canary -> wave -> full rollout 같은 단계적 확장 지원.
- 사전 검증(precheck) 실패 시 자동 취소.

#### 14.4.2 스케줄 상태 모델

| 상태 | 설명 |
|---|---|
| `pending` | 예약 저장, 실행 대기 |
| `precheck_passed` | 대상/아티팩트/충돌 검증 완료 |
| `executing` | 실제 적용 중 |
| `completed` | 적용 완료 |
| `failed` | 일부/전체 적용 실패 |
| `cancelled` | 운영자가 취소 |

#### 14.4.3 멱등성/정합성 규칙

- 모든 배포 명령은 `command_id`를 가진다.
- 서버는 `command_id` 중복 실행을 거부한다 (idempotent apply).
- `run_at_utc`는 UTC epoch ms로 저장한다.
- 허용 드리프트(`max_clock_skew_ms`)를 넘으면 실행 대신 `failed(clock_skew)`로 마킹한다.

### 14.5 동일 영역 충돌 안전장치/정책

여러 플러그인/스크립트가 같은 영역을 조작할 때 충돌을 예방하기 위해, 로드 전(pre-apply) 정적 검증과 런타임 중재를 함께 적용한다.

#### 14.5.1 실행 계층 정책

| 계층 | 목적 | 터미널 결정 허용 |
|---|---|---|
| `pre_validate` | 요청/입력 검증 | `deny`, `block` |
| `mutate` | 입력/출력 변환 | `modify` |
| `authorize` | 권한/정책 결정 | `allow`, `deny` |
| `side_effect` | 알림/기록/후처리 | `handled` (제한적) |
| `observe` | 관측/메트릭 | 없음 (`pass`만) |

#### 14.5.2 충돌 방지 규칙

1. 동일 `hook_scope + stage + exclusive_group` 조합에는 활성 artifact를 1개만 허용한다.
2. 동일 stage에서는 `priority` 오름차순으로 실행한다.
3. 터미널 결정 우선순위는 `block/deny > handled > modify > pass`로 고정한다.
4. `observe` stage는 상태 변경 결정을 반환할 수 없다.
5. 충돌이 탐지되면 적용 전에 배포를 차단한다 (precheck failure).

#### 14.5.3 서버별 정책 계층 (MMORPG 대비)

서버별/샤드별 정책 차이를 위해 설정 계층을 고정한다:

`global -> game_mode -> region -> shard -> server`

- 하위 계층이 상위 계층을 override할 수 있다.
- 단, `exclusive_group` 충돌 규칙은 모든 계층에서 동일하게 강제한다.

### 14.6 템플릿 + 초보자 가이드

초보자도 30분 내에 첫 artifact를 작성/적용할 수 있도록 템플릿과 문서를 제공한다.

#### 14.6.1 템플릿 파일

| 유형 | 경로(제안) | 내용 |
|---|---|---|
| 네이티브 플러그인 템플릿 | `server/plugins/templates/chat_hook_v2_template.cpp` | ABI v2 기본 함수 + 에러 처리 + 버퍼 작성 예시 |
| Lua 스크립트 템플릿 | `server/scripts/templates/on_join_template.lua` | ctx 입력/결정 반환/로그 패턴 |
| manifest 템플릿 (plugin) | `server/plugins/templates/plugin_manifest.template.json` | hook_scope/stage/priority/exclusive_group 예시 |
| manifest 템플릿 (script) | `server/scripts/templates/script_manifest.template.json` | 동일 |

#### 14.6.2 문서/도구

- 문서:
  - `docs/extensibility/plugin-quickstart.md`
  - `docs/extensibility/lua-quickstart.md`
  - `docs/extensibility/conflict-policy.md`
  - `docs/extensibility/recipes.md`
- 스캐폴드 도구:
  - `tools/new_plugin.py --name spam_filter --hook on_chat_send`
  - `tools/new_script.py --name on_join_policy --hook on_join`

### 14.7 도입 우선순위

1. **충돌 정책 + manifest 도입** (안전성 선행)
2. **파일 인벤토리 스캔 + 읽기 전용 콘솔**
3. **즉시 적용(타겟 지정) 배포**
4. **예약 교체 + canary/wave rollout**
5. **템플릿/가이드/스캐폴드 도구 배포**

우선순위 이유:

- 충돌 정책이 없는 상태에서 콘솔 배포 기능을 먼저 열면 운영 사고 가능성이 높다.
- 인벤토리/검증을 먼저 구축해야 예약 배포의 실패율을 낮출 수 있다.
