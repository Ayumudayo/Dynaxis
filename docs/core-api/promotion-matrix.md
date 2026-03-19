# Public Engine Promotion Matrix

## Scope
- This document fixes the current promotion decisions for the public `server_core` boundary.
- It is the canonical answer for which remaining `Internal` / `Transitional` seams are promoted now versus intentionally held back.
- Decision vocabulary:
  - `promote`: public engine contract now
  - `stay transitional`: publicly visible but still stabilization-in-progress
  - `stay internal`: app-owned or implementation-bound for now

## Tranche Summary
- `promote`: runtime composition seam (`EngineContext`, `EngineRuntime`, `EngineBuilder`) plus the already-stable host/signal/runtime host surface
- `promote`: plugin/Lua extensibility mechanism seam
- `stay internal`: gameplay-frequency transport/session internals, shared topology/discovery adapters, generic storage/unit-of-work seam

## Matrix

| Seam | Candidate surface | Decision | Reason |
|---|---|---|---|
| Runtime composition / lifecycle | `server/core/app/app_host.hpp`, `server/core/app/engine_builder.hpp`, `server/core/app/engine_context.hpp`, `server/core/app/engine_runtime.hpp`, `server/core/app/termination_signals.hpp` | `promote` | the bootstrap contract is now app-neutral, used by `server_app`, `gateway_app`, `admin_app`, and `wb_worker`, and no longer depends on app-local headers |
| Gameplay-frequency transport/session | accept loop, session runtime state, session implementation, and low-level canary RUDP engine pieces | `stay internal` | these surfaces still encode server/gateway session wiring or experimental/canary RUDP details rather than a consumer-safe engine transport contract |
| Shared topology / discovery | discovery record, registry adapter, and backend-factory seams | `stay internal` | canonical ownership moved into `core`, but the adapter/factory/selector semantics are still tied to current stack topology and do not yet have stable consumer governance |
| Generic storage / unit-of-work | connection-pool, async DB worker, Redis client, and unit-of-work SPI seams | `stay internal` | the SPI exists, but package consumers still lack a settled factory/repository story and the current adapters remain stack-specific |
| Plugin / Lua extensibility governance | `server/core/plugin/shared_library.hpp`, `server/core/plugin/plugin_host.hpp`, `server/core/plugin/plugin_chain_host.hpp`, `server/core/scripting/script_watcher.hpp`, `server/core/scripting/lua_runtime.hpp`, `server/core/scripting/lua_sandbox.hpp` | `promote` | the mechanism is reusable, has dedicated public-api and installed-consumer proof, and stays separated from chat-specific ABI/binding contracts |

## Notes
- Stable networking primitives that were already public (`connection`, `dispatcher`, `hive`, `listener`) remain public; this matrix is about unresolved promotion decisions, not about re-litigating already-stable headers.
- No discovery or storage seam is promoted just because ownership moved under `core`; promotion requires a consumer-safe contract and compatibility policy, not only code location.
- Realtime public capability work should promote a narrower transport/runtime contract above the current internal session/RUDP internals rather than exposing these implementation headers directly.
  - current promoted runtime slice: `server/core/realtime/runtime.hpp`
  - current promoted transport-policy slice: `server/core/realtime/direct_delivery.hpp`
  - `server/core/realtime/**` is the only public realtime naming; old fps wrapper paths are removed in 3.0
- Compatibility wrappers and chat-specific extensibility contracts remain the intentionally public non-stable seams.
