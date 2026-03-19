# Extensibility Governance

이 문서는 plugin/Lua extensibility를 `server_core`의 **platform capability**로 다루기 위한 현재 거버넌스 규칙을 정의한다.

현재 상태와 entrypoint는 `docs/extensibility/overview.md`를 기준으로 본다.

핵심 분류:

- `core/include/server/core/plugin/*`, `core/include/server/core/scripting/*`는 서비스 재사용을 위한 메커니즘 계층이다.
- `server/include/server/chat/chat_hook_plugin_abi.hpp`, `server/src/chat/*`, `server/src/scripting/chat_lua_bindings.cpp`는 첫 번째 concrete consumer인 chat service 계층이다.
- 현재 단계에서 extensibility는 app convenience가 아니라 **stable core mechanism + transitional service contract**로 본다.

## 1. Capability Boundary

### 1.1 Core-owned mechanism layer

- `shared_library.hpp`: dynamic library load/unload RAII
- `plugin_host.hpp`: single-plugin load/reload/cache-copy/validator host
- `plugin_chain_host.hpp`: multi-plugin chain orchestration and directory scan
- `script_watcher.hpp`: script/file reload watcher with lock/sentinel semantics
- `lua_sandbox.hpp`: Lua library allowlist and resource-limit policy
- `lua_runtime.hpp`: script load/call/reload, host API registration, metrics

### 1.2 Service-owned policy layer

- chat plugin ABI versions and hook payload shapes
- chat plugin chain decision semantics and metrics naming
- chat Lua host bindings and allowed actions
- control-plane rollout policy and hook conflict policy

## 2. Compatibility Levels

### 2.1 Stable

- `core/include/server/core/plugin/shared_library.hpp`
- `core/include/server/core/plugin/plugin_host.hpp`
- `core/include/server/core/plugin/plugin_chain_host.hpp`
- `core/include/server/core/scripting/script_watcher.hpp`
- `core/include/server/core/scripting/lua_sandbox.hpp`
- `core/include/server/core/scripting/lua_runtime.hpp`
- 이 메커니즘 헤더들은 external-consumer-oriented verification과 installed-package proof를 확보했으므로 일반 `Stable` API 규칙을 적용한다.

### 2.2 Transitional

- chat hook ABI, chat plugin chain policy, chat Lua bindings의 기본 level이다.
- API/ABI evolution is allowed, but must be documented, tested, and categorized as additive vs breaking.
- Any change to loader precedence, reload semantics, host-call meaning, or hook decision behavior must update the corresponding docs in the same change.

### 2.3 Internal

- Service-private helpers, test-only seams, and implementation details that should not be consumed as extensibility contracts.

## 3. Plugin ABI Governance

### 3.1 Versioning rules

- ABI versions are explicit numeric constants plus explicit entrypoints (for example `chat_hook_api_v2()`).
- Breaking ABI changes require a new version constant and a new entrypoint symbol rather than silent mutation of an existing struct or enum.
- Existing ABI entrypoints should remain supported for at least one release cycle after a successor is introduced.

### 3.2 Breaking changes

Treat these as breaking:

- removing or renaming exported entrypoints
- changing enum meaning or reusing existing enum values
- removing/reordering existing struct fields
- changing validator requirements for an existing ABI without version bump
- changing deny/modify/handled semantics in a way that breaks existing plugins

### 3.3 Additive changes

Allowed additive work inside a transitional phase:

- documentation clarification
- new tests or metrics that preserve existing meanings
- new optional behavior only when it does not change current loader/validator semantics

If an addition changes binary layout or required behavior, it must be treated as a new ABI version instead.

## 4. Lua Runtime Governance

### 4.1 Capability policy

- Official builds keep Lua capability included.
- Runtime activation remains controlled by `LUA_ENABLED` and script-directory configuration.
- `enabled()` reflects runtime state, not whether the binary was built with capability.

### 4.2 Script policy guarantees

- allowed libraries must be documented and intentionally limited
- instruction and memory limits are part of the runtime governance story, not incidental implementation
- auto-disable and reload recovery behavior must remain documented and tested
- function-style hooks with `ctx` are the primary authoring model; directive/return-table scaffolds remain fallback/testing aids only

### 4.3 Breaking changes

Treat these as breaking for transitional governance purposes:

- removing host API tables/functions used in documented samples
- renaming or deleting established `ctx` fields without migration notes
- changing sandbox defaults in a way that invalidates existing documented scripts
- changing hook-decision semantics or host-call error handling incompatibly

## 5. Staged Stabilization Plan

### Stage 1 - Mechanism hardening (completed)

- `plugin_host`, `plugin_chain_host`, `script_watcher`, `lua_sandbox`, and `lua_runtime` now have core tests plus stable public-api/package proof.
- service-specific ABI/binding logic stays out of core.

### Stage 2 - Service contract hardening

- Keep chat hook ABI and chat Lua bindings documented as service-owned contracts.
- Require server-level tests for deny/modify propagation, auto-disable, ordering, and sample artifacts.
- Keep control-plane rollout/conflict docs aligned with actual behavior.

### Stage 3 - External consumer proof (completed for core mechanism)

- `CorePublicApiExtensibilitySmoke` proves stable-header plugin load/chain plus Lua runtime/watcher behavior.
- `CoreInstalledPackageConsumer` runs `server_core_extensibility_consumer` to cover the installed package path.
- reusable core mechanism headers are `Stable`; service-specific chat contracts remain `Transitional`.

## 6. Migration-Note Policy

- Breaking extensibility changes require a migration note using `docs/core-api/migration-note-template.md`.
- The same change must update:
  - `docs/core-api/extensions.md`
  - any affected quickstart or operational doc
  - relevant tests covering the old/new behavior
- PRs that change ABI/runtime behavior must explicitly label the change as `additive`, `behavioral`, or `breaking`.

## 7. Required Evidence For Extensibility Changes

- changed docs updated in the same change
- relevant core/server tests passed
- if loader/runtime semantics changed, rollback or fallback behavior demonstrated
- if a compatibility claim changed, the compatibility matrix/boundary docs updated to match
