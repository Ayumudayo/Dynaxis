# PROJECT KNOWLEDGE BASE

Generated: 2026-02-17
Commit: 4a25352
Branch: Huge-Refactor

## Overview
Knights는 C++20 기반 분산 채팅 스택입니다: HAProxy(TCP) -> `gateway_app` -> `server_app`, 그리고 Redis Streams -> Postgres 경로를 비동기로 적재하는 `wb_worker`(write-behind)가 있습니다.

표준 런타임은 Linux(Docker) 풀스택(`docker/stack/`)이며, Windows는 개발(빌드/클라이언트) 용도로 둡니다.

## Structure
```
./
├─ core/                 # 공용 라이브러리(server_core)
├─ server/               # server_app(채팅 로직)
├─ gateway/              # gateway_app(엣지 세션/라우팅)
├─ tools/                # 코드 생성 + write-behind 도구/워커
├─ scripts/              # 빌드/도커 배치/스모크 스크립트
├─ docker/
│  ├─ stack/             # 풀스택 compose(권장)
│  └─ observability/     # Prometheus/Grafana 설정/대시보드
├─ docs/                 # 설계/운영 문서
├─ external/              # 3rd-party (imgui 등)
├─ Sapphire/              # 참고용 서브프로젝트(별도 레포; Knights 스택/빌드와 무관)
├─ proto/                # Protobuf 정의
├─ protocol/             # wire map(JSON) 등 프로토콜 소스
├─ tests/                # GTest + python 검증
└─ client_gui/           # (Windows) ImGui 기반 개발 클라이언트
```

## Code Map
| Symbol | Kind | Location | Role |
|---|---|---|---|
| `server::core::net::Hive` | class | `core/include/server/core/net/hive.hpp` | io_context 수명/실행 관리 |
| `server::core::metrics::MetricsHttpServer` | class | `core/include/server/core/metrics/http_server.hpp` | `/metrics` HTTP endpoint |
| `server::core::metrics::append_build_info` | fn | `core/include/server/core/metrics/build_info.hpp` | `knights_build_info` 메트릭 라인 출력 |
| `server::core::runtime_metrics::Snapshot` | struct | `core/include/server/core/runtime_metrics.hpp` | 프로세스 런타임 카운터 스냅샷 |
| `server::app::run_server` | fn | `server/src/app/bootstrap.cpp` | server_app 부트스트랩(설정/DI/리스너/루프) |
| `server::app::register_routes` | fn | `server/src/app/router.cpp` | opcode -> handler 라우팅 |
| `server::app::chat::ChatService` | class | `server/include/server/chat/chat_service.hpp` | 채팅 상태/핸들러 구현 |
| `server::core::plugin::PluginHost<ApiTable>` | class template | `core/include/server/core/plugin/plugin_host.hpp` | 단일 플러그인 로딩/리로드 host (cache-copy + validator) |
| `server::core::plugin::PluginChainHost<ApiTable>` | class template | `core/include/server/core/plugin/plugin_chain_host.hpp` | 멀티 플러그인 체인 host (디렉터리 스캔 + lock-free read) |
| `server::core::scripting::ScriptWatcher` | class | `core/include/server/core/scripting/script_watcher.hpp` | 스크립트/파일 mtime 감시 + sentinel/lock 지원 |
| `server::core::scripting::LuaRuntime` | class | `core/include/server/core/scripting/lua_runtime.hpp` | Lua 스크립트 로드/호출/메트릭 공용 런타임 |
| `server::app::chat::ChatHookPluginChain` | class | `server/src/chat/chat_hook_plugin_chain.hpp` | 채팅 훅 플러그인 체인(멀티 플러그인 + reload 폴링) |
| `server::app::chat::ChatHookPluginManager` | class | `server/src/chat/chat_hook_plugin_manager.hpp` | 단일 플러그인 로더(cache-copy + lock/sentinel) |
| `server::app::MetricsServer` | class | `server/src/app/metrics_server.cpp` | server_app `/metrics` endpoint |
| `gateway::GatewayApp` | class | `gateway/include/gateway/gateway_app.hpp` | 게이트웨이 라이프사이클/백엔드 선택 |
| `gateway::GatewayApp::run` | method | `gateway/src/gateway_app.cpp` | 게이트웨이 메인 루프(리스너/메트릭 포함) |
| `gateway::GatewayApp::BackendConnection` | class | `gateway/include/gateway/gateway_app.hpp` | backend 연결/브리지 세션(connect timeout + bounded send queue) |
| `gateway::GatewayConnection` | class | `gateway/include/gateway/gateway_connection.hpp` | 클라이언트<->백엔드 브리지 |
| `WbWorker` | class | `tools/wb_worker/main.cpp` | Redis Streams -> Postgres write-behind |
| `WbWorker::SleepDbReconnectBackoff` | method | `tools/wb_worker/main.cpp` | DB 장애 시 지수 백오프(+jitter) 재연결 대기 |

## Key Flows
- Client -> Stack: `haproxy`(TCP) -> `gateway_app` -> `server_app`.
- Gateway routing: sticky(`SessionDirectory`) -> least-connections(`InstanceRecord.active_sessions`) backend selection.
- Gateway backend guardrail: `GATEWAY_BACKEND_CONNECT_TIMEOUT_MS`, `GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES`로 connect 지연/송신 큐 폭주를 제한.
- Distributed fanout: `server_app` can `psubscribe` to `${REDIS_CHANNEL_PREFIX}fanout:*` for room broadcasts.
- Chat hook plugin(실험): `MSG_CHAT_SEND` 경로에 hot-reload 가능한 플러그인 체인을 적용(파일명 순서; cache-copy + 선택적 lock/sentinel).
- Runtime extensibility(실험): native v2 hook(login/join/leave/session/admin/chat) + Lua cold-hook(scaffold directive) 체인을 결합하고, `kBlock/kDeny`는 기본 경로를 중단한다.
- Plugin/script fallback: `CHAT_HOOK_FALLBACK_PLUGINS_DIR`, `LUA_FALLBACK_SCRIPTS_DIR`를 reload poll마다 재평가해 primary 경로가 비었을 때 자동 전환한다.
- Streams -> DB: `wb_emit`(`XADD`) -> `wb_worker`(`XREADGROUP`) -> Postgres `session_events`.
- wb_worker readiness/recovery: Redis+DB 의존성 정상화 전 `ready=false`, DB 장애 시 `WB_DB_RECONNECT_BASE_MS`/`WB_DB_RECONNECT_MAX_MS` 기반 지수 백오프 재연결.
- Metrics: each service exposes Prometheus text format on `/metrics` (ports wired in `docker/stack/docker-compose.yml`).

## Where To Look
| Task | Location | Notes |
|---|---|---|
| Build presets / 옵션 | `CMakeLists.txt`, `CMakePresets.json` | `BUILD_*` 옵션, Windows(vcpkg) vs Linux(docker) 흐름 |
| Windows 빌드 | `scripts/build.ps1` | vcpkg bootstrap + preset configure/build |
| Docker 풀스택 | `docker/stack/docker-compose.yml`, `scripts/deploy_docker.ps1` | `observability` profile 포함 |
| Observability | `docker/observability/prometheus/prometheus.yml`, `docker/observability/grafana/dashboards/`, `docs/ops/observability.md` | Grafana provisioning은 `docker/observability/grafana/provisioning/`; gateway/wb_worker 신규 하드닝 메트릭 포함 |
| 서버 런타임 메트릭 | `core/include/server/core/runtime_metrics.hpp`, `server/src/app/metrics_server.cpp` | `/metrics` 텍스트 포맷 노출 |
| 코어 플러그인/스크립팅 인프라 | `core/include/server/core/plugin/`, `core/include/server/core/scripting/`, `core/src/plugin/`, `core/src/scripting/` | `server`/`gateway`가 재사용하는 공용 host 계층 |
| Chat hook plugin | `server/src/chat/chat_hook_plugin_*.{hpp,cpp}`, `server/plugins/` | 설정: `docs/configuration.md`, `server/README.md` |
| Lua cold-hook scaffold | `core/src/scripting/lua_runtime.cpp`, `server/src/scripting/chat_lua_bindings.cpp`, `server/scripts/` | 현재는 directive/return-table 기반 scaffold 실행 |
| Runtime extensibility docs | `docs/runtime-extensibility-plan.md`, `docs/extensibility/`, `docs/core-api/extensions.md` | quickstart/recipes/정책 및 ABI 계약 |
| 게이트웨이 라우팅/세션 | `gateway/src/gateway_app.cpp`, `gateway/src/gateway_connection.cpp`, `gateway/README.md` | Redis Instance Registry + SessionDirectory + backend connect timeout/send queue guardrail |
| Write-behind 워커 | `tools/wb_worker/main.cpp`, `tools/wb_worker/README.md`, `docs/db/write-behind.md` | Redis Streams -> Postgres + `/metrics`(옵션) + DB reconnect backoff/readiness/drop visibility |
| DB 마이그레이션 | `tools/migrations/runner.cpp`, `tools/migrations/*.sql` | `CREATE INDEX CONCURRENTLY`는 트랜잭션 밖 |
| 프로토콜 코드 생성 | `core/protocol/system_opcodes.json`, `tools/gen_opcodes.py`, `protocol/wire_map.json`, `tools/gen_wire_codec.py` | 생성 대상: `core/include/server/core/protocol/system_opcodes.hpp`, `core/include/server/wire/codec.hpp` |
| Opcode 문서/검증 | `tools/gen_opcode_docs.py`, `docs/protocol/opcodes.md` | system/game 전체 16-bit 중복 검증 + CI 체크 |
| CI | `.github/workflows/ci.yml` | Windows 빌드/테스트 + Linux Docker 풀스택 스모크 + opcode check |
| 테스트 | `tests/CMakeLists.txt`, `scripts/smoke_wb.ps1` | `ctest --preset windows-test` |

## Commands
```powershell
# Windows dev build
pwsh scripts/build.ps1 -Config Debug -Target server_app
pwsh scripts/build.ps1 -Config Debug -Target gateway_app
pwsh scripts/build.ps1 -Config Debug -Target wb_worker

# Full stack (Docker) + Observability
pwsh scripts/run_full_stack_observability.ps1

# Down (Docker)
pwsh scripts/deploy_docker.ps1 -Action down -Observability

# Tests
ctest --preset windows-test

# Opcode spec/doc check
python tools/gen_opcode_docs.py --check
```

## Conventions
- Naming/namespace: `docs/naming-conventions.md` (역할 기반 네이밍, 파일/함수 snake_case, 코드 심볼 ASCII).
- Repo 구조 원칙/금지사항: `docs/repo-structure.md`.
- Headers: `.hpp` + `#pragma once`, include 순서(표준 -> 서드파티 -> 프로젝트).
- CMake: 소스는 명시적으로 나열(= `file(GLOB ...)` 금지). 옵션은 `BUILD_SERVER_STACK`, `BUILD_GATEWAY_APP`, `BUILD_SERVER_TESTS`, `BUILD_GTEST_TESTS`, `BUILD_CONTRACT_TESTS`, `BUILD_WRITE_BEHIND_TOOLS` 중심.
- Conan2: `conanfile.py` + `conan.lock` 기반으로 Windows 개발 의존성을 고정하며(리눅스/도커 런타임은 시스템 패키지 기반).
- Metrics: `/metrics`는 Prometheus text format. 포트는 `METRICS_PORT`로 제어(서비스별 환경 변수로 주입).

## Anti-Patterns (This Repo)
- 코드/타깃/산출물/네임스페이스에 `Knights/knights/kproj` 문자열 사용 금지(`docs/repo-structure.md`).
- CMake에서 소스 수집을 `GLOB`로 처리 금지(리뷰 불가/증분 빌드 혼선).
- `core/`가 `server/`/`gateway/` 구현에 의존하도록 만들지 말 것(단방향 의존).
- Docker runtime 실행은 `scripts/deploy_docker.ps1`(또는 `scripts/run_full_stack_observability.ps1`) 경로만 사용하고, 임의 wrapper를 추가하지 않는다.
- 비밀정보/개인정보를 로그에 남기지 말 것(운영/샘플 토큰 포함). `.env`는 커밋 금지.

## Notes
- Docker 이미지: `knights-base:latest`(기반) + 서비스별 런타임 이미지(`knights-server:local`, `knights-gateway:local`, `knights-worker:local`, `knights-admin:local`, `knights-migrator:local`). `docker/stack`은 `observability` profile로 exporters/Prometheus/Grafana를 포함.
- Dispatch latency quantile(p95/p99)는 최근 구간에 샘플이 없으면 NaN이 나올 수 있음(정상). 트래픽 주입 후 확인.
- Gateway 하드닝 기본값: `GATEWAY_BACKEND_CONNECT_TIMEOUT_MS=5000`, `GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES=262144`; 관련 장애 카운터는 `gateway_backend_*` 메트릭으로 노출.
- wb_worker는 Redis/DB 의존성 정상화 전 `ready=false`를 유지하고, DB 재연결 시 지수 백오프(+jitter)를 사용한다.
- `WB_ACK_ON_ERROR=1` + `WB_DLQ_ON_ERROR=0` 조합은 실패 이벤트 유실을 유발할 수 있으므로 운영에서 주의(관측: `wb_error_drop_total`).
- `Sapphire/`는 참고용으로 동봉된 별도 프로젝트이며, Knights의 런타임/빌드 대상으로 취급하지 않는다.
- clangd/LSP 정밀 진단이 필요하면 `pwsh scripts/configure_windows_ninja.ps1`로 `build-windows-ninja/compile_commands.json`를 생성한 뒤, repo root에 `compile_commands.json`를 두면 된다(파일은 `.gitignore` 처리됨).


## Workflow Orchestration

### 1. Self-Improvement Loop
- After ANY correction from the user: update 'tasks/lessons.md' with the pattern
- Write rules for yourself that prevent the same mistake
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start for relevant project

### 2. Verification Before Done
- Never mark a task complete without proving it works
- Diff behavior between main and your changes when relevant
- Ask yourself: "Would a staff engineer approve this?"
- Run tests, check logs, demonstrate correctness

### 3. Demand Elegance (Balanced)
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: "Knowing everything I know now, implement the elegant solution"
- Skip this for simple, obvious fixes - don't over-engineer
- Challenge your own work before presenting it

### 4. Autonomous Bug Fixing
- When given a bug report: just fix it. Don't ask for hand-holding
- Point at logs, errors, failing tests - then resolve them
- Zero context switching required from the user
- Go fix failing CI tests without being told how

## Task Management

1. ** Plan First **: Write plan to 'tasks/todo.md' with checkable items
2. ** Verify Plan **: Check in before starting implementation
3. ** Track Progress **: Mark items complete as you go
4. ** Explain Changes **: High-level summary at each step
5. ** Document Results **: Add review section to 'tasks/todo.md'
6. ** Capture Lessons **: Update `tasks/lessons.md' after corrections

## Core Principles

- ** Simplicity First **: Make every change as simple as possible. Impact minimal code.
- ** No Laziness **: Find root causes. No temporary fixes. Senior developer standards.
- ** Minimal Impact **: Changes should only touch what's necessary. Avoid introducing bugs.
