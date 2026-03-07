# Loadgen UDP/RUDP Follow-up Plan

## Scope

- Source spec: `docs/tests/loadgen-next-steps.md`
- Existing design/context: `docs/tests/loadgen-plan.md`, `tools/loadgen/README.md`
- Existing implementation: `tools/loadgen/main.cpp`, `tools/loadgen/session_client.hpp`, `tools/loadgen/session_client.cpp`
- Active backlog hooks: `tasks/todo.md`, `tasks/quantitative-validation.md`

## Verified Constraints

- `stack_loadgen` is currently wired through `CMakeLists.txt:218` and only builds `tools/loadgen/main.cpp` plus `tools/loadgen/session_client.cpp`.
- Scenario loading, validation, running, reporting, and summary printing are currently all combined in `tools/loadgen/main.cpp`.
- `make_session_client(...)` in `tools/loadgen/session_client.cpp:532` only implements TCP today and throws for `udp` and `rudp`.
- The gateway sends `MSG_UDP_BIND_RES` automatically after opening the backend session in `gateway/src/gateway_connection.cpp:296`.
- UDP ingress requires a prior TCP-issued bind ticket and accepts `MSG_UDP_BIND_REQ` first in `gateway/src/gateway_app.cpp:2192`.
- Generated protocol policy currently marks only `MSG_UDP_BIND_REQ` and `MSG_UDP_BIND_RES` as UDP-capable in `server/include/server/protocol/game_opcodes.hpp:76`-`server/include/server/protocol/game_opcodes.hpp:79`; `MSG_LOGIN_REQ`, `MSG_JOIN_ROOM`, `MSG_CHAT_SEND`, `MSG_PING`, and `MSG_PONG` remain TCP-only.
- RUDP attach is session-gated and fallback-oriented in `gateway/src/gateway_app.cpp:2020`-`gateway/src/gateway_app.cpp:2166`, with canary/allowlist selection driven by `gateway/include/gateway/rudp_rollout_policy.hpp`.

## Implementation Plan

1. Scenario contract hardening
   - Add `schema_version` to the scenario model and require the current version explicitly.
   - Keep top-level `transport` as the default with `groups[].transport` override.
   - Move scenario types/loading/validation out of `tools/loadgen/main.cpp` into dedicated loadgen files.
   - Validate impossible combinations up front:
     - unsupported transport
     - invalid mode
     - group count mismatch
     - invalid rate values
     - `udp`/`rudp` groups must currently use `login_only` and `join_room=false`, because the verified protocol surface does not allow login/join/chat/ping over UDP yet.

2. UDP transport implementation
   - Extend `TcpSessionClient` to parse and expose the TCP-delivered UDP bind ticket (`MSG_UDP_BIND_RES`).
   - Add `UdpSessionClient` in `tools/loadgen/session_client.*` that:
     - connects/logs in over TCP using the existing flow
     - waits for the bind ticket on the TCP channel
     - sends `MSG_UDP_BIND_REQ` over a UDP socket to a configurable UDP port
     - records explicit bind success/failure stats
     - only supports `login_only` today; `join`, `chat`, and `ping` fail with explicit transport-boundary errors.

3. RUDP scaffold and visibility
   - Add `RudpSessionClient` in `tools/loadgen/session_client.*` using the same TCP bootstrap + UDP bind path.
   - Use the existing core `RudpEngine` to send a client HELLO and detect HELLO_ACK vs timeout/fallback.
   - Limit the current RUDP workload surface to `login_only` with explicit errors for unsupported operations.
   - Record attach result visibility in summary/report so rollout/fallback behavior is observable even before RUDP data workloads exist.

4. Reporting and sample assets
   - Expand the report with per-transport breakdown data instead of a single flat aggregate.
   - Keep the top-level `transports` list and add attach/bind counters for `udp` and `rudp`.
   - Update existing sample scenarios to carry `schema_version`.
   - Add at least one deterministic UDP sample scenario and one RUDP attach-visibility sample scenario.
   - Update `tools/loadgen/README.md`, `docs/tests/loadgen-plan.md`, `docs/tests/loadgen-next-steps.md`, `docs/tests.md`, `tasks/todo.md`, and `tasks/quantitative-validation.md` to reflect the new contract and current transport boundaries.

## Verification Plan

1. Static checks
   - `lsp_diagnostics` on all modified loadgen/docs files

2. Build
   - `pwsh scripts/build.ps1 -Config Release -Target stack_loadgen`

3. Regression/runtime validation
   - existing TCP scenarios:
     - `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/steady_chat.json --report build/loadgen/steady_chat.json`
     - `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/mixed_session_soak.json --report build/loadgen/mixed_session_soak.json`
   - new contract checks:
     - invalid scenario missing `schema_version` fails explicitly
     - invalid UDP/RUDP workload mode fails explicitly
   - new transport validation:
     - run at least one UDP attach scenario against a gateway configured with UDP enabled
     - run at least one RUDP attach scenario and verify attach/fallback visibility in report output

## Out of Scope

- richer scenario language
- distributed coordinator/worker design
- GUI integration
- pretending chat/ping/join already work over UDP/RUDP when protocol policy still blocks them
