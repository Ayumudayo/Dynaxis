# Load Generator

`stack_loadgen`은 기존 `haproxy -> gateway_app -> server_app` 경로를 대상으로 두고, transport-aware 시나리오를 하나의 headless binary로 실행하기 위한 도구다.

현재 구현 단계:

- `tcp`: login / join / chat / ping workload 지원
- `udp`: TCP bootstrap + UDP bind 이후 direct `ping` / `fps_input` proof 지원 (`login_only`, `ping`, `fps_input`)
- `rudp`: TCP bootstrap + UDP bind + RUDP HELLO 이후 direct `ping` / `fps_input` proof 지원 (`login_only`, `ping`, `fps_input`)

현재 확인된 경계:

- 생성된 opcode policy 기준으로 direct UDP/RUDP proof frame은 `MSG_UDP_BIND_REQ/RES`와 `MSG_PING`까지 확장되었다.
- direct gameplay-frequency proof frame은 `MSG_FPS_INPUT` ingress와 `MSG_FPS_STATE_DELTA` egress까지 확장되었다.
- 따라서 `udp` / `rudp` transport는 아직 `join`, `chat` workload를 실행하지 않는다.
- unsupported workload는 scenario validation 단계에서 명시적으로 실패한다.
- UDP/RUDP attach 검증은 gateway-local bind ticket을 사용하므로 HAProxy 대신 같은 gateway의 TCP+UDP 포트를 직접 지정해야 한다.
- direct ping proof는 request만 UDP/RUDP direct path를 사용하고, `MSG_PONG` response는 기존 TCP bridge 경로를 유지한다.
- direct FPS proof는 request만 `MSG_FPS_INPUT` direct path를 사용하고, reliable resync snapshot은 TCP bridge 경로를 유지한다.

## Build

Windows:

```powershell
pwsh scripts/build.ps1 -Config Release -Target stack_loadgen
```

Linux / Docker build dir:

```bash
cmake --build build-linux --target stack_loadgen
```

## Scenarios

- `tools/loadgen/scenarios/steady_chat.json`
  - 모든 세션이 동일 room에 join 후 steady chat echo latency를 측정
- `tools/loadgen/scenarios/mixed_session_soak.json`
  - `chat`, `ping`, `login_only` 세션을 섞어 mixed soak를 수행
- `tools/loadgen/scenarios/mixed_session_soak_long.json`
  - HAProxy frontend 경로에서 TCP mixed soak를 48세션 / 60초로 확장한 control sample
- `tools/loadgen/scenarios/mixed_direct_udp_soak.json`
  - direct same-gateway 경로에서 TCP workload + UDP attach 세션을 함께 유지하는 60초 baseline soak
- `tools/loadgen/scenarios/mixed_direct_udp_soak_long.json`
  - direct same-gateway 경로에서 TCP workload + UDP attach 세션을 48세션 / 120초로 확장한 장시간 sample
- `tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json`
  - direct same-gateway 경로에서 TCP workload + RUDP attach 세션을 48세션 / 120초로 유지하는 rollout rehearsal sample
- `tools/loadgen/scenarios/udp_attach_login_only.json`
  - TCP bootstrap 이후 UDP bind attach를 deterministic하게 검증
- `tools/loadgen/scenarios/rudp_attach_login_only.json`
  - TCP bootstrap 이후 RUDP HELLO attach success/fallback visibility를 검증
- `tools/loadgen/scenarios/udp_ping_only.json`
  - TCP bootstrap + UDP bind 이후 direct UDP `ping` proof를 deterministic하게 검증
- `tools/loadgen/scenarios/rudp_ping_only.json`
  - TCP bootstrap + UDP bind + RUDP attach/fallback 이후 direct RUDP `ping` proof를 deterministic하게 검증
- `tools/loadgen/scenarios/mixed_direct_udp_ping_soak.json`
  - direct same-gateway 경로에서 TCP workload + UDP direct ping 세션을 24세션 / 60초로 유지하는 gameplay-rate sample
- `tools/loadgen/scenarios/mixed_direct_rudp_ping_soak.json`
  - direct same-gateway 경로에서 TCP workload + RUDP direct ping 세션을 24세션 / 60초로 유지하는 gameplay-rate sample
- `tools/loadgen/scenarios/udp_fps_input_only.json`
  - TCP snapshot + direct UDP delta를 포함한 deterministic FPS input proof
- `tools/loadgen/scenarios/rudp_fps_input_only.json`
  - TCP snapshot + direct RUDP delta 또는 fallback/OFF TCP delta를 포함한 deterministic FPS input proof
- `tools/loadgen/scenarios/mixed_direct_udp_fps_soak.json`
  - direct same-gateway 경로에서 TCP workload + UDP FPS input 세션을 24세션 / 60초로 유지하는 gameplay-rate sample
- `tools/loadgen/scenarios/mixed_direct_rudp_fps_soak.json`
  - direct same-gateway 경로에서 TCP workload + RUDP FPS input 세션을 24세션 / 60초로 유지하는 gameplay-rate sample

필드:

- `schema_version` (`1` required)
- `sessions`
- `ramp_up_ms`
- `duration_ms`
- `room`
- `room_password`
- `unique_room_per_run`
- `message_bytes`
- `login_prefix`
- `connect_timeout_ms`
- `read_timeout_ms`
- `transport` (top-level default)
- `groups[]`

`groups[]` 예시:

```json
{
  "name": "chat",
  "transport": "tcp",
  "mode": "chat",
  "count": 24,
  "rate_per_sec": 2.0,
  "join_room": true
}
```

transport:

- `tcp`
- `udp`
- `rudp`

모드:

- `chat`
- `ping`
- `fps_input`
- `login_only`

## Run

TCP 예시:

```powershell
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/steady_chat.json `
  --report build/loadgen/steady_chat.json `
  --verbose
```

UDP/RUDP가 TCP와 다른 포트를 쓰는 환경에서는 `--udp-port`를 추가한다.

```powershell
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/udp_attach_login_only.json `
  --report build/loadgen/udp_attach_login_only.json
```

`--verbose`를 주면 UDP bind / RUDP attach trace를 stderr로 출력한다.

Docker stack against HAProxy frontend:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/mixed_session_soak.json `
  --report build/loadgen/mixed_session_soak.json
```

Long HAProxy TCP soak:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/mixed_session_soak_long.json `
  --report build/loadgen/mixed_session_soak_long.json
```

Direct same-gateway mixed TCP+UDP soak:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_udp_soak.json `
  --report build/loadgen/mixed_direct_udp_soak.host.json
```

Deterministic UDP ping proof:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/udp_ping_only.json `
  --report build/loadgen/udp_ping_only.host.json `
  --verbose
```

Long direct same-gateway mixed TCP+UDP soak:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_udp_soak_long.json `
  --report build/loadgen/mixed_direct_udp_soak_long.host.json
```

Mixed direct TCP+UDP ping soak:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_udp_ping_soak.json `
  --report build/loadgen/mixed_direct_udp_ping_soak.host.json
```

Long direct same-gateway mixed TCP+RUDP soak:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json `
  --report build/loadgen/mixed_direct_rudp_soak_long.host.json
```

Deterministic RUDP ping proof:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/rudp_ping_only.json `
  --report build/loadgen/rudp_ping_only.host.json `
  --verbose
```

Mixed direct TCP+RUDP ping soak:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_ping_soak.json `
  --report build/loadgen/mixed_direct_rudp_ping_soak.host.json
```

Long direct same-gateway mixed TCP+RUDP fallback policy:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-fallback.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json `
  --report build/loadgen/mixed_direct_rudp_soak_long.fallback.host.json
```

Long direct same-gateway mixed TCP+RUDP OFF policy:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-off.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json `
  --report build/loadgen/mixed_direct_rudp_soak_long.off.host.json
```

RUDP attach visibility example:

```powershell
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/rudp_attach_login_only.json `
  --report build/loadgen/rudp_attach_login_only.json
```

Forced fallback visibility example:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-fallback.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/rudp_attach_login_only.json `
  --report build/loadgen/rudp_attach_login_only.fallback.json `
  --verbose
```

RUDP OFF invariance example:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-off.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/rudp_attach_login_only.json `
  --report build/loadgen/rudp_attach_login_only.off.json `
  --verbose
```

Same-network Docker bridge example:

```bash
docker run --rm --network "dynaxis-stack_dynaxis-stack" \
  -v "$(pwd):/workspace" \
  -w /workspace \
  dynaxis-base:latest \
  bash -lc "cmake --preset linux-release -DBUILD_SERVER_TESTS=OFF -DBUILD_GTEST_TESTS=OFF -DBUILD_CONTRACT_TESTS=OFF >/tmp/loadgen-configure.log && cmake --build build-linux --target stack_loadgen >/tmp/loadgen-build.log && ./build-linux/stack_loadgen --host gateway-1 --port 6000 --udp-port 7000 --scenario tools/loadgen/scenarios/udp_attach_login_only.json --report build/loadgen/udp_attach_login_only.trace.json --verbose"
```

Linux host-network container example:

```bash
docker run --rm --network host \
  -v "$(pwd):/workspace" \
  -w /workspace \
  dynaxis-base:latest \
  bash -lc "./build-linux/stack_loadgen --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_udp_fps_soak.json --report build/loadgen/mixed_direct_udp_fps_soak.hostnet.json"
```

UDP-only netem rehearsal example:

```bash
python tests/python/verify_fps_netem_rehearsal.py --scenario fps-pair
```

## Report

실행 결과는 JSON으로 남는다. 핵심 필드:

- `connected_sessions`
- `authenticated_sessions`
- `joined_sessions`
- `success_count`
- `error_count`
- `operations.chat_*`
- `operations.ping_*`
- `throughput_rps`
- `latency_ms.p50/p95/p99/max`
- `transport.connect_failures/read_timeouts/disconnects`
- `transport.udp_bind_*`
- `transport.rudp_attach_*`
- `transport_breakdown.<transport>.stats`

`transport.rudp_attach_fallbacks` is a visibility counter. In a forced-fallback scenario it is an expected success-path outcome, not an error by itself.

## Notes

- `gateway_app` 경로는 접속 직후 `MSG_LOGIN_REQ`를 기다리므로, loadgen은 `HELLO`를 선행 조건으로 두지 않는다.
- 커스텀 room을 여러 세션이 공유하려면 `room_password`를 지정하는 편이 안전하다. 현재 서버 정책상 초기에 만들어진 room은 owner/invite 제약이 생길 수 있다.
- 기본값으로 `unique_room_per_run=true`를 사용해 run마다 room name에 seed suffix를 붙여 이전 run의 room history/state와 분리한다.
- login ID도 run seed suffix를 포함하므로, 같은 scenario를 여러 프로세스로 동시에 실행해도 duplicate-login 충돌이 overload 신호로 오인되지 않는다.
- chat rate는 기본 spam/mute 임계값 아래로 유지해야 한다. 제공된 샘플 시나리오는 이 기준을 반영한다.
- UDP attach run prerequisites:
  - gateway에 `GATEWAY_UDP_LISTEN`과 `GATEWAY_UDP_BIND_SECRET`가 설정돼 있어야 한다.
  - direct UDP ping proof를 쓰려면 `GATEWAY_UDP_OPCODE_ALLOWLIST`에 `0x0012,0x0002`가 포함돼 있어야 한다.
  - `docker/stack/.env.rudp-*.example` 파일의 `GATEWAY_UDP_BIND_SECRET=replace-with-non-empty-secret`는 실제 실행 전에 non-empty 값으로 교체해야 한다.
- RUDP attach success prerequisites:
  - `GATEWAY_RUDP_ENABLE=1`
  - `GATEWAY_RUDP_CANARY_PERCENT=100`
  - non-empty `GATEWAY_RUDP_OPCODE_ALLOWLIST`
- direct ping proof uses `MSG_PING (0x0002)` in both UDP/RUDP allowlists together with `MSG_UDP_BIND_REQ (0x0012)`
- `rudp_ping_only.json`은 `RUDP` fallback/OFF에서도 실제 ping iteration이 실행되도록 10초 duration을 사용한다.
- RUDP forced fallback example:
  - `docker/stack/.env.rudp-fallback.example`
  - expected summary shape: `rudp_attach_ok=0 rudp_attach_fallback>0 errors=0`
- RUDP OFF invariance example:
  - `docker/stack/.env.rudp-off.example`
  - expected summary shape: `rudp_attach_ok=0 rudp_attach_fallback>0 errors=0`
- 2026-03-07 verification snapshot:
  - same-network UDP attach: `udp_bind_ok=4 udp_bind_fail=0`
  - same-network RUDP attach: `rudp_attach_ok=4 rudp_attach_fallback=0`
  - Windows host-path direct same-gateway UDP/RUDP attach도 동일하게 통과
  - forced fallback proof: `rudp_attach_ok=0 rudp_attach_fallback=4 errors=0`
  - direct mixed TCP+UDP soak proof: `success=283 errors=0 udp_bind_ok=4 udp_bind_fail=0 throughput_rps=4.48 p95_ms=12.06`
  - long HAProxy TCP soak proof: `success=639 errors=0 throughput_rps=9.64 p95_ms=12.83`
  - long direct mixed TCP+UDP soak proof: `success=1128 errors=0 udp_bind_ok=8 udp_bind_fail=0 throughput_rps=8.93 p95_ms=12.26`
  - long direct mixed TCP+RUDP soak proof: `success=1134 errors=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=8 rudp_attach_fallback=0 throughput_rps=8.98 p95_ms=12.08`
  - long direct mixed TCP+RUDP fallback proof: `success=1130 errors=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=8 throughput_rps=8.96 p95_ms=14.21`
  - long direct mixed TCP+RUDP OFF proof: `success=1131 errors=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=8 throughput_rps=8.96 p95_ms=12.95`
  - full matrix env restore proof: `build/loadgen/mixed_direct_rudp_soak_long.final.matrix.host.json`, `build/loadgen/rudp_attach_login_only.final.matrix.host.json`
  - RUDP OFF invariance proof: `rudp_attach_ok=0 rudp_attach_fallback=4 errors=0`
- 2026-03-16 FPS ping-path verification snapshot:
  - direct UDP attach regression: `udp_bind_ok=4 udp_bind_fail=0 errors=0`
  - direct RUDP attach regression: `udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=4 rudp_attach_fallback=0 errors=0`
  - `udp_ping_only`: `success=21 errors=0 udp_bind_ok=4 throughput_rps=3.49 p95_ms=1.50`
  - `rudp_ping_only` success path: `success=37 errors=0 udp_bind_ok=4 rudp_attach_ok=4 throughput_rps=3.36 p95_ms=78.65`
  - `mixed_direct_udp_ping_soak`: `success=518 errors=0 udp_bind_ok=4 throughput_rps=8.19 p95_ms=11.89`
  - `mixed_direct_rudp_ping_soak` success path: `success=501 errors=0 udp_bind_ok=4 rudp_attach_ok=4 throughput_rps=7.93 p95_ms=78.26`
  - `rudp_ping_only` fallback path: `success=6 errors=0 udp_bind_ok=4 rudp_attach_ok=0 rudp_attach_fallback=4 throughput_rps=0.28 p95_ms=1.43`
  - `mixed_direct_rudp_ping_soak` fallback path: `success=469 errors=0 udp_bind_ok=4 rudp_attach_ok=0 rudp_attach_fallback=4 throughput_rps=7.43 p95_ms=11.95`
  - `rudp_ping_only` OFF path: `success=6 errors=0 udp_bind_ok=4 rudp_attach_ok=0 rudp_attach_fallback=4 throughput_rps=0.28 p95_ms=1.32`
  - `mixed_direct_rudp_ping_soak` OFF path: `success=467 errors=0 udp_bind_ok=4 rudp_attach_ok=0 rudp_attach_fallback=4 throughput_rps=7.39 p95_ms=11.83`
  - retained metrics/log artifacts live under `build/engine-roadmap-fps/metrics/` and `build/engine-roadmap-fps/logs/`
- 2026-03-18 FPS direct-path soak verification snapshot:
  - `mixed_direct_udp_fps_soak`: `success=508 errors=0 udp_bind_ok=4 fps_direct_updates=222 throughput_rps=8.04 p95_ms=31.62`
    - report: `build/loadgen/mixed_direct_udp_fps_soak.20260318-010307Z.host.json`
  - `mixed_direct_rudp_fps_soak`: `success=507 errors=0 udp_bind_ok=4 rudp_attach_ok=4 fps_direct_updates=218 throughput_rps=8.02 p95_ms=31.60`
    - report: `build/loadgen/mixed_direct_rudp_fps_soak.20260318-010307Z.host.json`
- 2026-03-18 Phase 5 budget hardening snapshot:
  - `mixed_session_soak_long`: `success=636 errors=0 throughput_rps=9.60 p95_ms=15.04`
    - report: `build/loadgen/mixed_session_soak_long.20260318-021023Z.json`
    - note: on this Windows workstation, host `:6000` collides with a local X server, so the control sample was captured from a same-network Linux container against `haproxy:6000`
  - `mixed_direct_udp_soak_long`: `success=1126 errors=0 udp_bind_ok=8 throughput_rps=8.91 p95_ms=13.62`
    - report: `build/loadgen/mixed_direct_udp_soak_long.20260318-021023Z.host.json`
  - `mixed_direct_rudp_soak_long` success path: `success=1126 errors=0 udp_bind_ok=8 rudp_attach_ok=8 throughput_rps=8.92 p95_ms=16.96`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.host.json`
  - `mixed_direct_rudp_soak_long` focused success-path rerun: `success=1126 errors=0 udp_bind_ok=8 rudp_attach_ok=8 throughput_rps=8.92 p95_ms=13.40`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260317-173024Z.host.json`
    - note: the focused `--capture-set rudp-success-only` rerun confirmed that the earlier `p95_ms=16.96` sample was bounded variance rather than a new steady-state baseline
  - `mixed_direct_rudp_soak_long` fallback path: `success=1124 errors=0 udp_bind_ok=8 rudp_attach_fallback=8 throughput_rps=8.91 p95_ms=12.51`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.fallback.host.json`
  - `mixed_direct_rudp_soak_long` OFF path: `success=1126 errors=0 udp_bind_ok=8 rudp_attach_fallback=8 throughput_rps=8.92 p95_ms=12.54`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.off.host.json`
  - `mixed_direct_udp_fps_soak` rerun: `success=509 errors=0 udp_bind_ok=4 fps_direct_updates=219 throughput_rps=8.06 p95_ms=31.57`
    - report: `build/loadgen/mixed_direct_udp_fps_soak.20260318-021023Z.host.json`
  - `mixed_direct_rudp_fps_soak` rerun: `success=509 errors=0 udp_bind_ok=4 rudp_attach_ok=4 fps_direct_updates=220 throughput_rps=8.05 p95_ms=31.62`
    - report: `build/loadgen/mixed_direct_rudp_fps_soak.20260318-021023Z.host.json`
- 2026-03-18 Linux hostnet hardening snapshot:
  - `mixed_session_soak_long`: `success=637 errors=0 throughput_rps=9.61 p95_ms=13.37`
    - report: `build/loadgen/mixed_session_soak_long.20260318-060310Z.json`
  - `mixed_direct_udp_soak_long`: `success=1132 errors=0 udp_bind_ok=8 throughput_rps=8.97 p95_ms=12.78`
    - report: `build/loadgen/mixed_direct_udp_soak_long.20260318-060310Z.host.json`
  - `mixed_direct_rudp_soak_long` success path: `success=1125 errors=0 udp_bind_ok=8 rudp_attach_ok=8 throughput_rps=8.91 p95_ms=12.22`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260318-060310Z.host.json`
  - `mixed_direct_udp_fps_soak`: `success=511 errors=0 udp_bind_ok=4 fps_direct_updates=221 throughput_rps=8.08 p95_ms=31.97`
    - report: `build/loadgen/mixed_direct_udp_fps_soak.20260318-060310Z.host.json`
  - `mixed_direct_rudp_fps_soak`: `success=508 errors=0 udp_bind_ok=4 rudp_attach_ok=4 fps_direct_updates=219 throughput_rps=8.03 p95_ms=32.32`
    - report: `build/loadgen/mixed_direct_rudp_fps_soak.20260318-060310Z.host.json`
  - note: this host-network container path is the current scheduled/manual Linux hardening artifact path; same-network bridge container mode remains diagnostic only because it widens FPS `p95_ms`
  - focused OFF-path history:
    - `build/loadgen/mixed_direct_rudp_soak_long.20260318-113911Z-off1.off.host.json`: `success=1127 errors=0 throughput_rps=8.93 p95_ms=12.00`
    - `build/loadgen/mixed_direct_rudp_soak_long.20260318-113911Z-off2.off.host.json`: `success=1123 errors=0 throughput_rps=8.90 p95_ms=13.32`
    - `build/loadgen/mixed_direct_rudp_soak_long.20260318-113911Z-off3.off.host.json`: `success=1125 errors=0 throughput_rps=8.91 p95_ms=12.13`
    - note: current hostnet OFF-path history does not justify a second Linux-only numeric band
- 2026-03-18 OS-level UDP netem rehearsal snapshot:
  - artifact: `build/phase5-evidence/20260318-121332Z/netem/manifest.json`
  - `mixed_direct_udp_fps_soak`: `success=502 errors=0 udp_bind_ok=4 throughput_rps=7.94 p95_ms=75.25`
    - report: `build/loadgen/mixed_direct_udp_fps_soak.20260318-121332Z.netem.json`
  - `mixed_direct_rudp_fps_soak`: `success=505 errors=0 udp_bind_ok=4 rudp_attach_ok=4 throughput_rps=7.99 p95_ms=74.71`
    - report: `build/loadgen/mixed_direct_rudp_fps_soak.20260318-121332Z.netem.json`
  - metric deltas: `gateway_udp_loss_estimated_total +4`, `gateway_udp_jitter_ms_last -> 37`
  - note: this is a manual ops-only rehearsal path that shapes UDP egress inside the loadgen container; it is intentionally separate from the accepted Phase 5 baseline and `ci-hardening`
- current FPS first-slice contract:
  - direct UDP/RUDP proof frame: `MSG_PING`
  - response path for that proof: TCP `MSG_PONG`
