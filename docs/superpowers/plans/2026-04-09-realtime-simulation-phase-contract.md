# Realtime Simulation Phase Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add one narrowly scoped, engine-neutral `server::core::realtime` extension: a deterministic simulation phase contract that can be observed through `WorldRuntime` without promoting game rules into core.

**Architecture:** Introduce a new stable public header that defines simulation phase vocabulary and a small observer contract, then wire `WorldRuntime::tick()` to emit ordered phase events using captured snapshots rather than exposing mutable internals. Keep the first slice observational only; do not add combat, respawn, objective, or match logic.

**Tech Stack:** C++20, existing `server::core::realtime` public API, GTest/CTest contract proofs, Doxygen coverage checks, core API governance docs.

---

### Task 1: Publish The Stable Simulation-Phase Contract

**Files:**
- Create: `core/include/server/core/realtime/simulation_phase.hpp`
- Modify: `docs/core-api-boundary.md`
- Modify: `docs/core-api/realtime.md`
- Modify: `docs/core-api/overview.md`
- Modify: `docs/ops/realtime-runtime-contract.md`
- Modify: `core/include/server/core/api/version.hpp`
- Modify: `docs/core-api/compatibility-matrix.json`
- Modify: `tests/core/public_api_headers_compile.cpp`
- Modify: `tests/core/public_api_realtime_capability_smoke.cpp`
- Modify: `tests/core/public_api_stable_header_scenarios.cpp`

- [x] **Step 1: Add failing contract-smoke coverage for the new stable header**

Add direct includes/usages in the public compile/smoke files so the contract is required by the compile sweep, the realtime smoke, and the stable-header scenarios.

```cpp
#include "server/core/realtime/simulation_phase.hpp"

server::core::realtime::SimulationPhaseContext phase_context{
    .server_tick = 1,
    .actor_count = 0,
    .viewer_count = 0,
    .staged_input_count = 0,
    .replication_update_count = 0,
};
(void)phase_context;
```

- [x] **Step 2: Build the affected public API targets and confirm the build fails because the header/contract does not exist yet**

Run:

```powershell
cmake --build build-windows-release --config Release --target core_public_api_headers_compile core_public_api_stable_header_scenarios core_public_api_realtime_capability_smoke --parallel
```

Expected: the build fails because `server/core/realtime/simulation_phase.hpp` does not exist yet.

- [x] **Step 3: Run the governance-only contract checks and confirm they still point at the current stable inventory**

Run:

```powershell
ctest --preset windows-test --output-on-failure -R "CoreApiStableGovernanceFixtures"
```

Expected: governance checks pass against the current baseline, showing the failure is specifically in the missing new contract/build coverage rather than unrelated test drift.

- [x] **Step 4: Create the new public contract header**

Start with a narrow observational contract only.

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace server::core::realtime {

enum class SimulationPhase : std::uint8_t {
    kTickBegin = 0,
    kInputsApplied,
    kActorsAdvanced,
    kReplicationComputed,
    kTickEnd,
};

struct SimulationPhaseContext {
    std::uint32_t server_tick{0};
    std::size_t actor_count{0};
    std::size_t viewer_count{0};
    std::size_t staged_input_count{0};
    std::size_t replication_update_count{0};
};

class ISimulationPhaseObserver {
public:
    virtual ~ISimulationPhaseObserver() = default;
    virtual void on_simulation_phase(SimulationPhase phase, const SimulationPhaseContext& context) = 0;
};

} // namespace server::core::realtime
```

- [x] **Step 5: Publish the contract in governance/docs**

Update:
- `docs/core-api-boundary.md`
- `docs/core-api/realtime.md`
- `docs/core-api/overview.md`
- `docs/ops/realtime-runtime-contract.md`
- `docs/core-api/compatibility-matrix.json`
- `core/include/server/core/api/version.hpp`

Rules:
- treat the new header as `[Stable]`
- bump the API version additively
- describe it as phase vocabulary / observational hook only
- explicitly state that it does not own gameplay rules

- [x] **Step 6: Rebuild the affected public API targets**

Run:

```powershell
cmake --build build-windows-release --config Release --target core_public_api_headers_compile core_public_api_stable_header_scenarios core_public_api_realtime_capability_smoke --parallel
```

Expected: build succeeds.

- [x] **Step 7: Re-run the contract proof set**

Run:

```powershell
ctest --preset windows-test --output-on-failure -R "CorePublicApiHeadersCompile|CorePublicApiRealtimeCapabilitySmoke|CorePublicApiStableHeaderScenarios|CoreApiStableGovernanceFixtures"
```

Expected: all selected tests pass.

- [ ] **Step 8: Commit the contract-publication slice**

```bash
git add core/include/server/core/realtime/simulation_phase.hpp core/include/server/core/api/version.hpp docs/core-api-boundary.md docs/core-api/realtime.md docs/core-api/overview.md docs/ops/realtime-runtime-contract.md docs/core-api/compatibility-matrix.json tests/core/public_api_headers_compile.cpp tests/core/public_api_realtime_capability_smoke.cpp tests/core/public_api_stable_header_scenarios.cpp
git commit -m "feat: publish realtime simulation phase contract"
```

### Task 2: Wire Ordered Phase Emission Into `WorldRuntime`

**Files:**
- Modify: `core/include/server/core/realtime/runtime.hpp`
- Modify: `core/src/realtime/runtime.cpp`
- Modify: `tests/server/test_fps_runtime.cpp`
- Modify: `tests/core/public_api_realtime_capability_smoke.cpp`

- [x] **Step 1: Add a failing runtime test for deterministic phase order**

Add a recorder observer and assert exact order/context for one tick.

```cpp
class RecordingPhaseObserver final : public server::core::realtime::ISimulationPhaseObserver {
public:
    void on_simulation_phase(server::core::realtime::SimulationPhase phase,
                             const server::core::realtime::SimulationPhaseContext& context) override {
        events.emplace_back(phase, context.server_tick, context.replication_update_count);
    }

    std::vector<std::tuple<server::core::realtime::SimulationPhase, std::uint32_t, std::size_t>> events;
};

TEST(FpsWorldRuntimeTest, EmitsSimulationPhasesInDeterministicOrder) {
    RecordingPhaseObserver observer;
    server::core::realtime::WorldRuntime runtime;
    runtime.set_simulation_phase_observer(&observer);
    (void)runtime.stage_input(1, server::core::realtime::InputCommand{.input_seq = 1});

    const auto updates = runtime.tick();
    ASSERT_FALSE(updates.empty());
    ASSERT_EQ(observer.events.size(), 5u);
    EXPECT_EQ(std::get<0>(observer.events[0]), server::core::realtime::SimulationPhase::kTickBegin);
    EXPECT_EQ(std::get<0>(observer.events[4]), server::core::realtime::SimulationPhase::kTickEnd);
}
```

- [x] **Step 2: Build the affected runtime targets and confirm the build fails**

Run:

```powershell
cmake --build build-windows-release --config Release --target server_fps_tests core_public_api_realtime_capability_smoke --parallel
```

Expected: build fails because `WorldRuntime` does not yet expose the observer API or emit phase events.

- [x] **Step 3: Run the focused runtime test selection using actual discovered test names**

Run:

```powershell
ctest --preset windows-test --output-on-failure -R "FpsWorldRuntimeTest|CorePublicApiRealtimeCapabilitySmoke"
```

Expected: the selected test names either fail to build earlier or, once built, fail because the observer API/phase emissions are missing.

- [x] **Step 4: Add the observer API to `WorldRuntime`**

Extend the public header narrowly.

```cpp
#include "server/core/realtime/simulation_phase.hpp"

class WorldRuntime {
public:
    void set_simulation_phase_observer(ISimulationPhaseObserver* observer) noexcept;
    // existing API...
private:
    ISimulationPhaseObserver* simulation_phase_observer_{nullptr};
};
```

- [x] **Step 5: Implement ordered phase emission without exposing mutable internals**

Implementation rules:
- do not call observer callbacks while exposing mutable internal containers
- capture phase contexts as value snapshots
- preserve existing tick semantics
- keep the observer optional and zero-cost when unset

Suggested shape:

```cpp
void WorldRuntime::set_simulation_phase_observer(ISimulationPhaseObserver* observer) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    simulation_phase_observer_ = observer;
}

std::vector<ReplicationUpdate> WorldRuntime::tick() {
    std::vector<std::pair<SimulationPhase, SimulationPhaseContext>> phase_events;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // existing tick work
        // push phase_events in exact order with captured counts
    }
    if (observer != nullptr) {
        for (const auto& [phase, context] : phase_events) {
            observer->on_simulation_phase(phase, context);
        }
    }
    return outbound;
}
```

- [x] **Step 6: Extend the realtime smoke to exercise the observer API**

Add a tiny observer in `tests/core/public_api_realtime_capability_smoke.cpp` and prove it receives at least one phase callback during `runtime.tick()`.

- [x] **Step 7: Rebuild the affected runtime targets**

Run:

```powershell
cmake --build build-windows-release --config Release --target server_fps_tests core_public_api_realtime_capability_smoke --parallel
```

Expected: build succeeds.

- [x] **Step 8: Re-run the focused runtime proof**

Run:

```powershell
ctest --preset windows-test --output-on-failure -R "FpsWorldRuntimeTest|CorePublicApiRealtimeCapabilitySmoke"
```

Expected: selected tests pass, including the new phase-order assertion.

- [ ] **Step 9: Commit the runtime wiring slice**

```bash
git add core/include/server/core/realtime/runtime.hpp core/src/realtime/runtime.cpp tests/server/test_fps_runtime.cpp tests/core/public_api_realtime_capability_smoke.cpp
git commit -m "feat: emit deterministic realtime simulation phases"
```

### Task 3: Final Proof And Documentation Policy Checks

**Files:**
- Modify: `tasks/architecture/cod-bf-style-fps-guidelines-2026-04-09/todo.md`

- [x] **Step 1: Re-read the boundary checklist against the finished diff**

Verify the final implementation still obeys:
- no gameplay managers in `core`
- observational contract only
- additive stable API change only

- [x] **Step 2: Rebuild all affected targets for the final proof**

Run:

```powershell
cmake --build build-windows-release --config Release --target core_public_api_headers_compile core_public_api_stable_header_scenarios core_public_api_realtime_capability_smoke server_fps_tests --parallel
```

Expected: build succeeds.

- [x] **Step 3: Run the complete proof set for this slice**

Run:

```powershell
ctest --preset windows-test --output-on-failure -R "CorePublicApiHeadersCompile|CorePublicApiStableHeaderScenarios|CorePublicApiRealtimeCapabilitySmoke|FpsWorldRuntimeTest|CoreApiStableGovernanceFixtures|DoxygenCoverageToolTests"
```

Expected: all selected tests pass.

- [x] **Step 4: Run explicit Doxygen coverage verification**

Run:

```powershell
python tools/check_doxygen_coverage.py
```

Expected: no missing public-header documentation issues for the new realtime contract surface.

- [x] **Step 5: Record the verification result in the local task note**

Add a concise review entry with:
- exact commands run
- whether the new stable header was added
- confirmation that no gameplay-rule types entered `core`

- [ ] **Step 6: Commit the final docs/proof bookkeeping**

```bash
git add tasks/architecture/cod-bf-style-fps-guidelines-2026-04-09/todo.md
git commit -m "docs: record realtime simulation phase proof"
```
