# Load Generator

`stack_loadgen`은 기존 `haproxy -> gateway_app -> server_app` 경로를 대상으로 두고, transport-aware 시나리오를 하나의 headless binary로 실행하기 위한 도구다.

현재 구현 단계:

- `tcp`: login / join / chat / ping workload 지원
- `udp`: TCP bootstrap + UDP bind attach 검증 지원 (`login_only`만 허용)
- `rudp`: TCP bootstrap + UDP bind + RUDP HELLO attach visibility 지원 (`login_only`만 허용)

현재 확인된 경계:

- 생성된 opcode policy 기준으로 `MSG_UDP_BIND_REQ/RES`만 UDP-capable이다.
- 따라서 `udp` / `rudp` transport는 아직 `join`, `chat`, `ping` workload를 실행하지 않는다.
- unsupported workload는 scenario validation 단계에서 명시적으로 실패한다.
- UDP/RUDP attach 검증은 gateway-local bind ticket을 사용하므로 HAProxy 대신 같은 gateway의 TCP+UDP 포트를 직접 지정해야 한다.

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
  - `docker/stack/.env.rudp-*.example` 파일의 `GATEWAY_UDP_BIND_SECRET=replace-with-non-empty-secret`는 실제 실행 전에 non-empty 값으로 교체해야 한다.
- RUDP attach success prerequisites:
  - `GATEWAY_RUDP_ENABLE=1`
  - `GATEWAY_RUDP_CANARY_PERCENT=100`
  - non-empty `GATEWAY_RUDP_OPCODE_ALLOWLIST`
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
- 현재 protocol policy 기준으로 UDP-capable opcode는 `MSG_UDP_BIND_REQ/RES`만 존재한다.
