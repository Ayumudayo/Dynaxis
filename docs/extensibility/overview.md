# Extensibility Overview

Dynaxis currently treats native plugin/Lua extensibility as a reusable platform capability, not as a one-off chat feature.
The reusable core mechanism is now governed as `Stable`, while chat-specific ABI/bindings remain `Transitional`.

## Current Capability

- native plugin loading/reload is provided through `server_core` plugin hosts
- Lua cold-hook execution, sandboxing, reload, and metrics are provided through `server_core` scripting primitives
- `server_app` is the first concrete consumer and exposes chat-oriented hook bindings plus admin/control-plane rollout paths
- official builds include the capability; runtime activation is controlled by `CHAT_HOOK_ENABLED` and `LUA_ENABLED`

## Ownership Boundary

- `core/include/server/core/plugin/*` and `core/include/server/core/scripting/*` own the reusable mechanism layer
- service-owned ABI, hook payload semantics, and chat-specific bindings stay under `server/`
- hot-path behavior remains native-first; Lua is intended for cold-hook policy/customization paths
- function-style Lua hooks (`function on_<hook>(ctx)`) are the primary authoring model

## Operational Expectations

- reload, sandbox limits, auto-disable, and fallback behavior are part of the supported runtime story, not incidental implementation details
- rollout/conflict semantics for artifact deployment are documented through the admin/control-plane contract
- changes that affect ABI/runtime behavior must update the relevant docs and tests in the same change

## Verification Pointers

- server tests under `tests/server/` cover hook integration and auto-disable behavior
- core tests under `tests/core/` cover plugin host, chain host, Lua runtime, and Lua sandbox behavior
- `CorePublicApiExtensibilitySmoke` covers stable-header plugin load/chain plus Lua runtime/watcher behavior
- `CoreInstalledPackageConsumer` runs `server_core_extensibility_consumer` from the installed package path
- Python admin/control-plane verification lives in `tests/python/verify_admin_api.py`, `verify_admin_auth.py`, and `verify_admin_read_only.py`

## Related Docs

- `docs/extensibility/governance.md`
- `docs/extensibility/control-plane-api.md`
- `docs/extensibility/conflict-policy.md`
- `docs/extensibility/plugin-quickstart.md`
- `docs/extensibility/lua-quickstart.md`
- `docs/extensibility/recipes.md`
- `docs/core-api/extensions.md`
