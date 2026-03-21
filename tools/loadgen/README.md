# 부하 생성기(loadgen)

`stack_loadgen`은 기존 `haproxy -> gateway_app -> server_app` 경로를 대상으로, 전송 계층을 인식하는 시나리오를 하나의 무인 실행 바이너리로 돌리기 위한 도구다.

이 도구가 필요한 이유는 단순하다. 수동 테스트만으로는 TCP 경로와 direct UDP/RUDP 경로가 실제 부하에서 어떻게 달라지는지 반복해서 확인하기 어렵기 때문이다. `stack_loadgen`은 같은 시나리오를 여러 번 재현해, 기능 회귀와 성능 예산을 함께 확인하게 해 준다.

초보자 관점에서는 이 도구를 두 가지 용도로 나눠 이해하는 것이 가장 쉽다.

- 기능 검증
  - attach, `ping_only`, `fps_input_only`처럼 "이 경로가 실제로 붙는가"를 확인하는 시나리오
- 부하/예산 검증
  - `mixed_*_soak*.json`처럼 "붙은 뒤에 어느 정도 지연(latency)과 처리량(throughput)이 나오는가"를 확인하는 시나리오

이 구분이 중요한 이유는, attach가 성공했다고 해서 성능 예산이 만족된 것은 아니고, 반대로 soak가 느리다고 해서 attach 자체가 실패한 것도 아니기 때문이다. 문서를 읽을 때도 "연결 검증"과 "부하 검증"을 분리해서 보는 편이 훨씬 덜 헷갈린다.

현재 구현 단계:

- `tcp`: login / join / chat / ping workload 지원
- `udp`: TCP bootstrap + UDP bind 이후 direct `ping` / `fps_input` 검증을 지원한다 (`login_only`, `ping`, `fps_input`)
- `rudp`: TCP bootstrap + UDP bind + RUDP HELLO 이후 direct `ping` / `fps_input` 검증을 지원한다 (`login_only`, `ping`, `fps_input`)

현재 확인된 경계:

- 생성된 opcode policy 기준으로 direct UDP/RUDP proof frame은 `MSG_UDP_BIND_REQ/RES`와 `MSG_PING`까지 확장되었다.
- direct 고빈도 게임플레이 검증 프레임은 `MSG_FPS_INPUT` ingress와 `MSG_FPS_STATE_DELTA` egress까지 확장되었다.
- 따라서 `udp` / `rudp` transport는 아직 `join`, `chat` workload를 실행하지 않는다.
- 미지원 workload는 시나리오 검증 단계에서 명시적으로 실패한다.
- UDP/RUDP attach 검증은 gateway-local bind ticket을 사용하므로 HAProxy 대신 같은 gateway의 TCP+UDP 포트를 직접 지정해야 한다.
- direct ping proof는 request만 UDP/RUDP direct path를 사용하고, `MSG_PONG` response는 기존 TCP bridge 경로를 유지한다.
- direct FPS proof는 request만 `MSG_FPS_INPUT` direct path를 사용하고, reliable resync snapshot은 TCP bridge 경로를 유지한다.

즉, `stack_loadgen`은 "모든 기능을 똑같이 때려 보는 만능 도구"가 아니다. 현재 저장소가 실제로 열어 둔 전송 계약을 정확히 따라가며, 아직 공개 표면(public surface)과 런타임 경계가 고정되지 않은 기능은 억지로 검증하지 않는다. 이 보수적인 범위 제한 덕분에 문서와 도구가 함께 어긋나는 드리프트(drift)를 줄일 수 있다.

## 빌드

Windows 빌드:

```powershell
pwsh scripts/build.ps1 -Config Release -Target stack_loadgen
```

Linux 또는 Docker 빌드 디렉터리:

```bash
cmake --build build-linux --target stack_loadgen
```

## 시나리오

아래 설명에서는 JSON 파일 이름은 그대로 두고, 의미 설명만 한국어로 적는다. 파일명과 scenario key는 자동화/스크립트에서 그대로 쓰이므로 영문 식별자를 유지하는 편이 유지보수에 더 안전하다.

- `tools/loadgen/scenarios/steady_chat.json`
  - 모든 세션이 같은 room에 들어간 뒤 안정 구간 chat echo 지연을 측정한다
- `tools/loadgen/scenarios/mixed_session_soak.json`
  - `chat`, `ping`, `login_only` 세션을 섞어 기본 혼합 soak를 수행한다
- `tools/loadgen/scenarios/mixed_session_soak_long.json`
  - HAProxy 프런트엔드 경로에서 TCP 혼합 soak를 48세션 / 60초로 확장한 대조군이다
- `tools/loadgen/scenarios/mixed_direct_udp_soak.json`
  - 같은 gateway 직접 경로에서 TCP workload + UDP attach 세션을 함께 유지하는 60초 기준선 soak다
- `tools/loadgen/scenarios/mixed_direct_udp_soak_long.json`
  - 같은 gateway 직접 경로에서 TCP workload + UDP attach 세션을 48세션 / 120초로 확장한 장시간 샘플이다
- `tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json`
  - 같은 gateway 직접 경로에서 TCP workload + RUDP attach 세션을 48세션 / 120초로 유지하는 롤아웃 리허설용 샘플이다
- `tools/loadgen/scenarios/udp_attach_login_only.json`
  - TCP bootstrap 뒤에 UDP bind attach가 붙는지 결정적으로 검증한다
- `tools/loadgen/scenarios/rudp_attach_login_only.json`
  - TCP bootstrap 뒤에 RUDP HELLO attach 성공 여부와 fallback 가시성을 검증한다
- `tools/loadgen/scenarios/udp_ping_only.json`
  - TCP bootstrap + UDP bind 뒤에 direct UDP `ping` 경로를 결정적으로 검증한다
- `tools/loadgen/scenarios/rudp_ping_only.json`
  - TCP bootstrap + UDP bind + RUDP attach/fallback 뒤에 direct RUDP `ping` 경로를 결정적으로 검증한다
- `tools/loadgen/scenarios/mixed_direct_udp_ping_soak.json`
  - 같은 gateway 직접 경로에서 TCP workload + UDP direct ping 세션을 24세션 / 60초로 유지하는 게임플레이 빈도 샘플이다
- `tools/loadgen/scenarios/mixed_direct_rudp_ping_soak.json`
  - 같은 gateway 직접 경로에서 TCP workload + RUDP direct ping 세션을 24세션 / 60초로 유지하는 게임플레이 빈도 샘플이다
- `tools/loadgen/scenarios/udp_fps_input_only.json`
  - TCP snapshot + direct UDP delta를 포함한 FPS input 경로를 결정적으로 검증한다
- `tools/loadgen/scenarios/rudp_fps_input_only.json`
  - TCP snapshot + direct RUDP delta 또는 fallback/OFF TCP delta를 포함한 FPS input 경로를 결정적으로 검증한다
- `tools/loadgen/scenarios/mixed_direct_udp_fps_soak.json`
  - 같은 gateway 직접 경로에서 TCP workload + UDP FPS input 세션을 24세션 / 60초로 유지하는 게임플레이 빈도 샘플이다
- `tools/loadgen/scenarios/mixed_direct_rudp_fps_soak.json`
  - 같은 gateway 직접 경로에서 TCP workload + RUDP FPS input 세션을 24세션 / 60초로 유지하는 게임플레이 빈도 샘플이다

주요 필드:

아래 키 이름은 scenario JSON의 실제 스키마이므로 영문 그대로 유지한다. 대신 "무슨 의미인지"를 같이 적는다.

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

지원 전송 방식:

- `tcp`
- `udp`
- `rudp`

지원 동작 모드:

- `chat`
- `ping`
- `fps_input`
- `login_only`

## 실행

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

HAProxy 프런트엔드를 통한 Docker 스택 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/mixed_session_soak.json `
  --report build/loadgen/mixed_session_soak.json
```

장시간 HAProxy TCP 부하 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/mixed_session_soak_long.json `
  --report build/loadgen/mixed_session_soak_long.json
```

같은 gateway 직접 경로의 혼합 TCP+UDP 부하 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_udp_soak.json `
  --report build/loadgen/mixed_direct_udp_soak.host.json
```

결정적 UDP ping 검증 예시:

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

장시간 같은 gateway 직접 경로의 혼합 TCP+UDP 부하 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_udp_soak_long.json `
  --report build/loadgen/mixed_direct_udp_soak_long.host.json
```

혼합 direct TCP+UDP ping 부하 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_udp_ping_soak.json `
  --report build/loadgen/mixed_direct_udp_ping_soak.host.json
```

장시간 같은 gateway 직접 경로의 혼합 TCP+RUDP 부하 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json `
  --report build/loadgen/mixed_direct_rudp_soak_long.host.json
```

결정적 RUDP ping 검증 예시:

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

혼합 direct TCP+RUDP ping 부하 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_ping_soak.json `
  --report build/loadgen/mixed_direct_rudp_ping_soak.host.json
```

장시간 같은 gateway 직접 경로의 혼합 TCP+RUDP fallback 정책 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-fallback.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json `
  --report build/loadgen/mixed_direct_rudp_soak_long.fallback.host.json
```

장시간 같은 gateway 직접 경로의 혼합 TCP+RUDP OFF 정책 예시:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-off.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json `
  --report build/loadgen/mixed_direct_rudp_soak_long.off.host.json
```

RUDP attach 가시성 예시:

```powershell
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/rudp_attach_login_only.json `
  --report build/loadgen/rudp_attach_login_only.json
```

강제 fallback 가시성 예시:

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

RUDP OFF 불변성 예시:

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

동일 네트워크 Docker bridge 예시:

```bash
docker run --rm --network "dynaxis-stack_dynaxis-stack" \
  -v "$(pwd):/workspace" \
  -w /workspace \
  dynaxis-base:latest \
  bash -lc "cmake --preset linux-release -DBUILD_SERVER_TESTS=OFF -DBUILD_GTEST_TESTS=OFF -DBUILD_CONTRACT_TESTS=OFF >/tmp/loadgen-configure.log && cmake --build build-linux --target stack_loadgen >/tmp/loadgen-build.log && ./build-linux/stack_loadgen --host gateway-1 --port 6000 --udp-port 7000 --scenario tools/loadgen/scenarios/udp_attach_login_only.json --report build/loadgen/udp_attach_login_only.trace.json --verbose"
```

Linux host-network 컨테이너 예시:

```bash
docker run --rm --network host \
  -v "$(pwd):/workspace" \
  -w /workspace \
  dynaxis-base:latest \
  bash -lc "./build-linux/stack_loadgen --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_udp_fps_soak.json --report build/loadgen/mixed_direct_udp_fps_soak.hostnet.json"
```

UDP 전용 `netem` 리허설 예시:

```bash
python tests/python/verify_fps_netem_rehearsal.py --scenario fps-pair
```

## 결과 보고서

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

`transport.rudp_attach_fallbacks`는 가시성 카운터다. 강제 fallback 시나리오에서는 실패가 아니라 "fallback이 의도대로 보였는가"를 확인하는 성공 경로 지표로 읽어야 한다.

결과를 읽을 때는 다음 순서를 권장한다.

1. 연결이 되었는가
   - `connected_sessions`, `authenticated_sessions`, `joined_sessions`
2. 기능 검증이 통과했는가
   - `success_count`, `error_count`, `transport.udp_bind_*`, `transport.rudp_attach_*`
3. 성능 예산을 넘지 않았는가
   - `throughput_rps`, `latency_ms.p95/p99`

이 순서가 중요한 이유는, 연결 자체가 실패한 실행에서 latency 수치만 보는 것이 큰 의미가 없기 때문이다. 항상 "붙었는지 -> 원하는 경로가 열렸는지 -> 그다음 예산이 어떤지" 순서로 읽는 편이 좋다.

## 운영 메모

- `gateway_app` 경로는 접속 직후 `MSG_LOGIN_REQ`를 기다리므로, loadgen은 `HELLO`를 선행 조건으로 두지 않는다.
- 커스텀 room을 여러 세션이 공유하려면 `room_password`를 지정하는 편이 안전하다. 현재 서버 정책상 초기에 만들어진 room은 owner/invite 제약이 생길 수 있다.
- 기본값으로 `unique_room_per_run=true`를 사용해 run마다 room name에 seed suffix를 붙여 이전 run의 room history/state와 분리한다.
- login ID도 run seed suffix를 포함하므로, 같은 scenario를 여러 프로세스로 동시에 실행해도 duplicate-login 충돌이 overload 신호로 오인되지 않는다.
- chat rate는 기본 spam/mute 임계값 아래로 유지해야 한다. 제공된 샘플 시나리오는 이 기준을 반영한다.
- UDP attach 실행 전제:
  - gateway에 `GATEWAY_UDP_LISTEN`과 `GATEWAY_UDP_BIND_SECRET`가 설정돼 있어야 한다.
  - direct UDP ping proof를 쓰려면 `GATEWAY_UDP_OPCODE_ALLOWLIST`에 `0x0012,0x0002`가 포함돼 있어야 한다.
  - `docker/stack/.env.rudp-*.example` 파일의 `GATEWAY_UDP_BIND_SECRET=replace-with-non-empty-secret`는 실제 실행 전에 non-empty 값으로 교체해야 한다.
- RUDP attach 성공 전제:
  - `GATEWAY_RUDP_ENABLE=1`
  - `GATEWAY_RUDP_CANARY_PERCENT=100`
  - non-empty `GATEWAY_RUDP_OPCODE_ALLOWLIST`
- direct ping 검증은 `MSG_UDP_BIND_REQ (0x0012)`와 함께 `MSG_PING (0x0002)`를 UDP/RUDP allowlist에 모두 넣어야 한다
- `rudp_ping_only.json`은 `RUDP` fallback/OFF에서도 실제 ping iteration이 실행되도록 10초 duration을 사용한다.
- RUDP 강제 fallback 예시:
  - `docker/stack/.env.rudp-fallback.example`
  - 기대 결과 형태: `rudp_attach_ok=0 rudp_attach_fallback>0 errors=0`
- RUDP OFF 불변성 예시:
  - `docker/stack/.env.rudp-off.example`
  - 기대 결과 형태: `rudp_attach_ok=0 rudp_attach_fallback>0 errors=0`
- 2026-03-07 검증 스냅샷:
  - 동일 네트워크 UDP attach: `udp_bind_ok=4 udp_bind_fail=0`
  - 동일 네트워크 RUDP attach: `rudp_attach_ok=4 rudp_attach_fallback=0`
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
- 2026-03-16 FPS ping-path 검증 스냅샷:
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
- 2026-03-18 Phase 5 예산 하드닝 스냅샷:
  - `mixed_session_soak_long`: `success=636 errors=0 throughput_rps=9.60 p95_ms=15.04`
    - report: `build/loadgen/mixed_session_soak_long.20260318-021023Z.json`
    - 메모: 이 Windows 워크스테이션에서는 host `:6000`이 로컬 X server와 충돌해, 대조군은 같은 네트워크의 Linux 컨테이너에서 `haproxy:6000`을 대상으로 수집했다
  - `mixed_direct_udp_soak_long`: `success=1126 errors=0 udp_bind_ok=8 throughput_rps=8.91 p95_ms=13.62`
    - report: `build/loadgen/mixed_direct_udp_soak_long.20260318-021023Z.host.json`
  - `mixed_direct_rudp_soak_long` success path: `success=1126 errors=0 udp_bind_ok=8 rudp_attach_ok=8 throughput_rps=8.92 p95_ms=16.96`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.host.json`
  - `mixed_direct_rudp_soak_long` focused success-path rerun: `success=1126 errors=0 udp_bind_ok=8 rudp_attach_ok=8 throughput_rps=8.92 p95_ms=13.40`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260317-173024Z.host.json`
    - 메모: focused `--capture-set rudp-success-only` 재실행으로, 앞선 `p95_ms=16.96` 샘플이 새로운 steady-state 기준선이라기보다 bounded variance였음을 확인했다
  - `mixed_direct_rudp_soak_long` fallback path: `success=1124 errors=0 udp_bind_ok=8 rudp_attach_fallback=8 throughput_rps=8.91 p95_ms=12.51`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.fallback.host.json`
  - `mixed_direct_rudp_soak_long` OFF path: `success=1126 errors=0 udp_bind_ok=8 rudp_attach_fallback=8 throughput_rps=8.92 p95_ms=12.54`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.off.host.json`
  - `mixed_direct_udp_fps_soak` rerun: `success=509 errors=0 udp_bind_ok=4 fps_direct_updates=219 throughput_rps=8.06 p95_ms=31.57`
    - report: `build/loadgen/mixed_direct_udp_fps_soak.20260318-021023Z.host.json`
  - `mixed_direct_rudp_fps_soak` rerun: `success=509 errors=0 udp_bind_ok=4 rudp_attach_ok=4 fps_direct_updates=220 throughput_rps=8.05 p95_ms=31.62`
    - report: `build/loadgen/mixed_direct_rudp_fps_soak.20260318-021023Z.host.json`
- 2026-03-18 Linux hostnet 하드닝 스냅샷:
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
  - 메모: 이 host-network 컨테이너 경로가 현재 scheduled/manual Linux 하드닝 artifact 경로다. 동일 네트워크 bridge 컨테이너 모드는 FPS `p95_ms`를 넓히므로 진단 전용으로만 남겨 둔다
  - focused OFF-path history:
    - `build/loadgen/mixed_direct_rudp_soak_long.20260318-113911Z-off1.off.host.json`: `success=1127 errors=0 throughput_rps=8.93 p95_ms=12.00`
    - `build/loadgen/mixed_direct_rudp_soak_long.20260318-113911Z-off2.off.host.json`: `success=1123 errors=0 throughput_rps=8.90 p95_ms=13.32`
    - `build/loadgen/mixed_direct_rudp_soak_long.20260318-113911Z-off3.off.host.json`: `success=1125 errors=0 throughput_rps=8.91 p95_ms=12.13`
    - 메모: 현재 hostnet OFF-path 이력만으로는 Linux 전용 두 번째 수치 band를 따로 둘 근거가 아직 없다
- 2026-03-18 OS 수준 UDP `netem` 리허설 스냅샷:
  - artifact: `build/phase5-evidence/20260318-121332Z/netem/manifest.json`
  - `mixed_direct_udp_fps_soak`: `success=502 errors=0 udp_bind_ok=4 throughput_rps=7.94 p95_ms=75.25`
    - report: `build/loadgen/mixed_direct_udp_fps_soak.20260318-121332Z.netem.json`
  - `mixed_direct_rudp_fps_soak`: `success=505 errors=0 udp_bind_ok=4 rudp_attach_ok=4 throughput_rps=7.99 p95_ms=74.71`
    - report: `build/loadgen/mixed_direct_rudp_fps_soak.20260318-121332Z.netem.json`
  - metric deltas: `gateway_udp_loss_estimated_total +4`, `gateway_udp_jitter_ms_last -> 37`
  - 메모: 이것은 loadgen 컨테이너 안쪽의 UDP egress를 shaping하는 수동 ops 전용 리허설 경로다. 승인된 Phase 5 기준선과 `ci-hardening`과는 의도적으로 분리해 둔다
- 현재 FPS 1차 계약:
  - direct UDP/RUDP proof frame: `MSG_PING`
  - response path for that proof: TCP `MSG_PONG`
