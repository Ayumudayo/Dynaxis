# 공개 엔진 승격 매트릭스

## 범위

- 이 문서는 public `server_core` 경계에서 어떤 seam을 지금 승격할지 고정한다.
- 아직 `Internal` 또는 `Transitional`로 남아 있는 surface 중 무엇을 지금 공개 계약으로 삼고, 무엇을 의도적으로 보류하는지에 대한 기준 문서다.

결정 vocabulary:

- `promote`
  - 지금 public engine contract로 승격
- `stay transitional`
  - 공개로 보이지만 아직 안정화 진행 중
- `stay internal`
  - 아직 app-owned 또는 implementation-bound

이 문서가 필요한 이유는 “코드가 core 아래로 이동했다”와 “이제 public API다”를 같은 의미로 오해하지 않게 하기 위해서다.

## 현재 tranche 요약

- `promote`
  - runtime composition seam (`EngineContext`, `EngineRuntime`, `EngineBuilder`)
  - 이미 안정화된 host/signal/runtime host surface
- `promote`
  - plugin/Lua extensibility mechanism seam
- `stay internal`
  - gameplay-frequency transport/session internals
  - shared topology/discovery adapters
  - generic storage/unit-of-work seam의 underlying implementation

## 매트릭스

| Seam | Candidate surface | 결정 | 이유 |
|---|---|---|---|
| Runtime composition / lifecycle | `server/core/app/app_host.hpp`, `server/core/app/engine_builder.hpp`, `server/core/app/engine_context.hpp`, `server/core/app/engine_runtime.hpp`, `server/core/app/termination_signals.hpp` | `promote` | bootstrap contract가 app-neutral해졌고 `server_app`, `gateway_app`, `admin_app`, `wb_worker`가 함께 쓰며 app-local header에 더 이상 의존하지 않기 때문 |
| Gameplay-frequency transport/session | accept loop, session runtime state, session implementation, low-level canary RUDP engine pieces | `stay internal` | 현재 구현은 server/gateway 세션 wiring과 experimental/canary RUDP 세부를 강하게 품고 있어 consumer-safe transport contract로 보기 어렵기 때문 |
| Shared topology / discovery | discovery record, registry adapter, backend-factory seams | `stay internal` | canonical ownership은 `core`로 이동했지만 adapter/factory/selector 의미가 아직 current stack topology에 묶여 있고 stable consumer governance가 부족하기 때문 |
| Generic storage / unit-of-work | connection-pool, async DB worker, Redis client, unit-of-work SPI seams | `stay internal` | SPI는 존재하지만 package consumer 관점의 factory/repository story가 아직 완전히 고정되지 않았고 concrete adapter도 stack-specific하기 때문 |
| Plugin / Lua extensibility governance | `server/core/plugin/shared_library.hpp`, `server/core/plugin/plugin_host.hpp`, `server/core/plugin/plugin_chain_host.hpp`, `server/core/scripting/script_watcher.hpp`, `server/core/scripting/lua_runtime.hpp`, `server/core/scripting/lua_sandbox.hpp` | `promote` | 메커니즘이 재사용 가능하고, dedicated public-api / installed-consumer proof가 있으며, chat-specific ABI/binding과 분리돼 있기 때문 |

## 메모

- 이미 public으로 운영 중인 networking primitive(`connection`, `dispatcher`, `hive`, `listener`)는 그대로 public이다. 이 매트릭스는 “아직 결론이 덜 난 승격 후보”를 정리하는 문서다.
- discovery나 storage가 `core` 아래에 있다는 이유만으로 public이 되지는 않는다. 승격에는 consumer-safe contract와 compatibility policy가 먼저 필요하다.
- realtime public capability는 current internal session/RUDP internals를 그대로 여는 대신, 더 좁고 안정적인 transport/runtime contract 위로 승격하는 방향이어야 한다.
  - 현재 승격된 runtime slice: `server/core/realtime/runtime.hpp`
  - 현재 승격된 transport-policy slice: `server/core/realtime/direct_delivery.hpp`
  - public realtime 기준 이름은 `server/core/realtime/**`뿐이며, old fps wrapper path는 3.0에서 제거되었다
- compatibility wrapper와 chat-specific extensibility contract는 의도적으로 “공개지만 비안정(non-stable)” 표면으로 남는다.
