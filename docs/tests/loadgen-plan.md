# Load Generator Plan

상태: transport-aware TCP/UDP/RUDP attach phase implemented and locally verified

브랜치: `feature/tcp-loadgen`

## 1. 목적

기존 서비스 경로

- `haproxy -> gateway_app -> server_app`

를 그대로 대상으로 두고, transport-aware 시나리오를 단일 headless load generator binary에서 실행할 수 있게 만든다. 현재 구현 단계에서는 `tcp` workload와 `udp` / `rudp` attach validation을 지원한다.

이 도구의 1차 목표는 다음과 같다.

- mixed session soak의 재현 가능한 실행 경로 확보
- steady chat / ping / login / join 부하의 latency / throughput 측정
- `tests/python/verify_soak_perf_gate.py`보다 높은 동시성, 더 긴 지속 시간, 더 명확한 결과 포맷 제공

## 2. 비목표

- 별도 synthetic server 구현
- UDP/RUDP data workload 전체 구현
- GUI 포함 클라이언트
- 운영형 distributed coordinator/worker 구조

초기 버전은 단일 프로세스, headless CLI로 제한하며, transport adapter 구조 아래에서 `tcp` workload와 `udp` / `rudp` attach visibility를 우선 구현한다.

## 3. 재사용 자산

### 3.1 프로토콜/프레이밍

- [packet.hpp](../../core/include/server/core/protocol/packet.hpp)
  - `PacketHeader`
  - `encode_header()`
  - `decode_header()`
  - `write_lp_utf8()`
- [system_opcodes.hpp](../../core/include/server/core/protocol/system_opcodes.hpp)
- [game_opcodes.hpp](../../server/include/server/protocol/game_opcodes.hpp)
- [codec.hpp](../../core/include/server/wire/codec.hpp)

### 3.2 기존 클라이언트 구현 참고

- [net_client.cpp](../../client_gui/src/net_client.cpp)
  - TCP 송수신 루프
  - 로그인/입장/채팅 요청 직렬화
  - `MSG_HELLO`, `MSG_ERR`, `MSG_LOGIN_RES`, `MSG_CHAT_BROADCAST` 처리
- [verify_chat.py](../../tests/python/verify_chat.py)
  - 최소 TCP smoke 시나리오
- [verify_soak_perf_gate.py](../../tests/python/verify_soak_perf_gate.py)
  - 현재 soak/perf gate 기준과 출력 형태

## 4. 배치 위치

- 새 도구 경로: `tools/loadgen/`
- 새 실행 파일 이름: `stack_loadgen`
- 새 문서/시나리오 경로:
  - `tools/loadgen/README.md`
  - `tools/loadgen/scenarios/*.json`

## 5. 빌드 정책

권장 정책:

- 루트 CMake에 `BUILD_LOADGEN_TOOLS` 옵션 추가
  - 기본값: `ON`
  - `BUILD_SERVER_STACK=ON`일 때만 허용
- 타깃은 `server_core`, `wire_proto`를 재사용한다.
- 새 외부 의존성은 추가하지 않는다.

이유:

- 기존 wire/protocol 자산을 그대로 재사용할 수 있다.
- 운영 바이너리와 독립적으로 빌드/배포할 수 있다.

## 6. 구조

초기 구조:

1. `session_client`
   - `SessionClient` interface
   - `TcpSessionClient` 구현
   - connect / login / join / chat / ping
   - `gateway_app` 경로 기준 immediate `LOGIN_REQ`

2. `scenario_runner`
   - 세션 수, ramp-up, duration, message rate 제어
   - group별 `transport` 선택
   - run별 unique room suffix 적용 가능

3. `metrics_collector`
   - latency samples
   - success/error count
   - reconnect / timeout count

4. `report_writer`
   - JSON summary 출력

## 7. 최소 시나리오

### 7.1 steady_chat

- N개 세션 로그인
- 동일 room 입장
- 일정 rate로 `MSG_CHAT_SEND`
- self-broadcast 또는 수신 확인

### 7.2 mixed_session_soak

- 일부 세션은 login only
- 일부 세션은 join + chat
- 일부 세션은 ping only
- duration 동안 steady-state 유지

### 7.3 mixed_session_soak_long

- `mixed_session_soak`의 장시간 / 대세션 control sample
- HAProxy frontend 기준 TCP workload baseline을 장시간으로 확보

### 7.4 mixed_direct_udp_soak_long

- direct same-gateway 경로에서 TCP workload + UDP attach를 함께 유지
- duration 동안 `udp_bind_failures=0`을 유지하는지 확인

### 7.5 mixed_direct_rudp_soak_long

- direct same-gateway 경로에서 TCP workload + RUDP attach를 함께 유지
- success-path rollout rehearsal baseline으로 사용

## 8. CLI 초안

예시:

```text
stack_loadgen --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/steady_chat.json --report build/loadgen/steady_chat.json
```

필수 인자:

- `--host`
- `--port`
- `--scenario`
- `--report`

선택 인자:

- `--udp-port`
- `--seed`
- `--verbose`

## 9. 시나리오 파일 초안

```json
{
  "name": "steady_chat",
  "sessions": 24,
  "ramp_up_ms": 3000,
  "duration_ms": 15000,
  "room": "loadgen_room",
  "room_password": "loadgen-shared",
  "unique_room_per_run": true,
  "message_bytes": 96,
  "login_prefix": "steady_chat",
  "groups": [
    {
      "name": "chat",
      "transport": "tcp",
      "mode": "chat",
      "count": 24,
      "rate_per_sec": 0.4,
      "join_room": true
    }
  ]
}
```

## 10. 리포트 포맷 초안

```json
{
  "scenario": "steady_chat",
  "room": "loadgen_room_<seed>",
  "transports": ["tcp"],
  "sessions": 64,
  "connected_sessions": 64,
  "authenticated_sessions": 64,
  "joined_sessions": 64,
  "success_count": 0,
  "error_count": 0,
  "throughput_rps": 0.0,
  "latency_ms": {
    "p50": 0.0,
    "p95": 0.0,
    "p99": 0.0,
    "max": 0.0
  },
  "transport": {
    "connect_failures": 0,
    "read_timeouts": 0,
    "disconnects": 0
  }
}
```

## 10.1 실제 wire 관찰 메모

- `haproxy -> gateway_app` 경로는 connect 직후 `MSG_HELLO`를 보장하지 않는다.
- handshake는 첫 `MSG_LOGIN_REQ`를 기다린 뒤 backend 연결을 열고 투명 브리지로 전환한다.
- join 성공 확인은 현 시점 기준 `MSG_STATE_SNAPSHOT`보다 시스템 `MSG_CHAT_BROADCAST`가 더 안정적이었다.
- 고정 room 재사용은 이전 run의 room history/state를 끌고 와 packet size/정책 이슈를 만들 수 있어 `unique_room_per_run`을 기본값으로 둔다.

## 11. 검증 계획

### 11.1 설계 검증

- 문서/CLI/리포트 포맷을 먼저 고정
- 기존 smoke/soak와 책임이 겹치지 않도록 구분

### 11.2 구현 검증

- 로컬 빌드: `stack_loadgen` 단독 빌드 성공
- 로컬 stack:
  - baseline connect/login/join/chat
  - JSON report 출력 확인
- 최소 회귀:
  - 1세션
  - 다중 세션
  - timeout/error path

### 11.3 운영형 검증

- Docker stack against HAProxy frontend
- `tasks/quantitative-validation.md`의 mixed soak 항목에 실제 수치 기록

### 11.4 실제 완료 검증

- `pwsh scripts/build.ps1 -Config Release -Target stack_loadgen`
- `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
- `python tests/python/verify_chat.py`
- `python tests/python/verify_pong.py`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/steady_chat.json --report build/loadgen/steady_chat.json`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/mixed_session_soak.json --report build/loadgen/mixed_session_soak.json`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/mixed_session_soak_long.json --report build/loadgen/mixed_session_soak_long.json`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/udp_attach_login_only.json --report build/loadgen/udp_attach_login_only.host.json --verbose`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/rudp_attach_login_only.json --report build/loadgen/rudp_attach_login_only.host.json --verbose`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_udp_soak_long.json --report build/loadgen/mixed_direct_udp_soak_long.host.json`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json --report build/loadgen/mixed_direct_rudp_soak_long.host.json`

실제 결과:

- `steady_chat`: `loadgen_summary scenario=steady_chat transports=tcp sessions=24 connected=24 authenticated=24 joined=24 success=155 errors=0 throughput_rps=8.60 p95_ms=14.41`
- `mixed_session_soak`: `loadgen_summary scenario=mixed_session_soak transports=tcp sessions=24 connected=24 authenticated=24 joined=12 success=85 errors=0 throughput_rps=4.67 p95_ms=12.66`
- `mixed_session_soak_long`: `loadgen_summary scenario=mixed_session_soak_long transports=tcp sessions=48 connected=48 authenticated=48 joined=24 success=639 errors=0 attach_failures=0 udp_bind_ok=0 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=0 throughput_rps=9.64 p95_ms=12.83`
- `udp_attach_login_only` (Windows host-path direct same-gateway): `loadgen_summary scenario=udp_attach_login_only transports=udp sessions=4 connected=4 authenticated=4 joined=0 success=0 errors=0 attach_failures=0 udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=0`
- `rudp_attach_login_only` (Windows host-path direct same-gateway): `loadgen_summary scenario=rudp_attach_login_only transports=rudp sessions=4 connected=4 authenticated=4 joined=0 success=0 errors=0 attach_failures=0 udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=4 rudp_attach_fallback=0`
- `mixed_direct_udp_soak` (Windows host-path direct same-gateway): `loadgen_summary scenario=mixed_direct_udp_soak transports=tcp,udp sessions=24 connected=24 authenticated=24 joined=10 success=283 errors=0 attach_failures=0 udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=0 throughput_rps=4.48 p95_ms=12.06`
- `mixed_direct_udp_soak_long` (Windows host-path direct same-gateway): `loadgen_summary scenario=mixed_direct_udp_soak_long transports=tcp,udp sessions=48 connected=48 authenticated=48 joined=20 success=1128 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=0 throughput_rps=8.93 p95_ms=12.26`
- `mixed_direct_rudp_soak_long` (Windows host-path direct same-gateway): `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1134 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=8 rudp_attach_fallback=0 throughput_rps=8.98 p95_ms=12.08`
- `mixed_direct_rudp_soak_long` fallback policy (Windows host-path direct same-gateway + `docker/stack/.env.rudp-fallback.example`): `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1130 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=8 throughput_rps=8.96 p95_ms=14.21`
- `mixed_direct_rudp_soak_long` OFF policy (Windows host-path direct same-gateway + `docker/stack/.env.rudp-off.example`): `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1131 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=8 throughput_rps=8.96 p95_ms=12.95`
- `mixed_direct_rudp_soak_long` env restore proof (Windows host-path direct same-gateway + `docker/stack/.env.rudp-attach.example`): `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1131 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=8 rudp_attach_fallback=0 throughput_rps=8.96 p95_ms=13.04`
- same-network Docker bridge verification:
  - `udp_attach_login_only`: `udp_bind_ok=4 udp_bind_fail=0 attach_failures=0`
  - `rudp_attach_login_only`: `udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=4 rudp_attach_fallback=0`
- forced fallback verification (Windows host-path direct same-gateway + `docker/stack/.env.rudp-fallback.example`): `loadgen_summary scenario=rudp_attach_login_only transports=rudp sessions=4 connected=4 authenticated=4 joined=0 success=0 errors=0 attach_failures=0 udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=4`
- RUDP OFF invariance verification (Windows host-path direct same-gateway + `docker/stack/.env.rudp-off.example`): `loadgen_summary scenario=rudp_attach_login_only transports=rudp sessions=4 connected=4 authenticated=4 joined=0 success=0 errors=0 attach_failures=0 udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=4`
- repeatability pass after the gateway UDP send fix:
  - `udp_attach_login_only.repeat1`: `udp_bind_ok=4 udp_bind_fail=0`
  - `rudp_attach_login_only.repeat1`: `rudp_attach_ok=4 rudp_attach_fallback=0`
  - `udp_attach_login_only.repeat2`: `udp_bind_ok=4 udp_bind_fail=0`
  - `rudp_attach_login_only.repeat2`: `rudp_attach_ok=4 rudp_attach_fallback=0`

후속 결함 처리:

- loadgen 반복 실행 중 드러난 `gateway_app` 종료 경로의 in-flight write buffer 수명 문제를 같은 브랜치에서 수정했다.
- 수정 후 same-stack repeated run(`steady -> mixed -> steady -> mixed`) 기준으로 gateway 컨테이너가 살아 있고 `double free` 로그가 재발하지 않음을 확인했다.
- UDP attach follow-up에서 gateway UDP bind response의 `async_send_to` 버퍼 수명 문제가 zero-byte datagram을 유발한다는 점을 추적 로그로 확인했고, send buffer lifetime을 수정한 뒤 same-network / host-path direct same-gateway 검증이 모두 deterministic하게 통과했다.
- `rudp_attach_fallback`는 forced-fallback scenario에서 기대되는 visibility counter이며, attach failure/error와 같은 의미로 해석하지 않는다.
- mixed quantitative baseline으로 `mixed_direct_udp_soak` sample을 추가했고, direct same-gateway 경로에서 60초 동안 `udp_bind_fail=0`를 유지하며 TCP workload throughput/latency를 함께 기록했다.
- 장시간 basic scenario 3종(`mixed_session_soak_long`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long`)을 추가해 HAProxy control sample과 direct same-gateway UDP/RUDP success-path 표본을 확대했다.
- mixed long RUDP sample은 success / fallback / OFF / env restore 4-way 표본까지 확보해 rollout/rollback 비교 기준으로 사용할 수 있다.

## 12. 현재 완료 기준

- `stack_loadgen`이 실제 stack에 TCP 세션 다수를 붙일 수 있다.
- `steady_chat`, `mixed_session_soak` 2개 시나리오를 실행할 수 있다.
- `mixed_session_soak_long`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long` 시나리오로 더 긴 / 더 큰 세션 표본을 재현할 수 있다.
- `mixed_direct_udp_soak` 시나리오로 direct same-gateway mixed TCP+UDP baseline을 재현할 수 있다.
- `udp_attach_login_only`, `rudp_attach_login_only` 시나리오가 direct same-gateway TCP+UDP 경로에서 deterministic하게 성공한다.
- `rudp_attach_login_only` 시나리오가 forced-fallback / OFF invariance 설정에서도 attach failure 없이 fallback visibility를 남긴다.
- JSON report에 latency/throughput/error 요약이 남는다.
- Docker stack 기준 실행 절차가 문서화된다.

## 13. 장기 후속

- 복잡한 분기/반복/장애 주입 시나리오를 JSON에 억지로 넣지 않는다.
- 현재는 schema-driven JSON을 유지하되, 제어 흐름 복잡도가 커지면 richer scenario language를 별도 장기 TODO로 검토한다.
