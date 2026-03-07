# Quantitative Validation Backlog

> Status (2026-03-07): Active measurement / benchmark backlog.
> Scope: 숫자 기반 판단이 필요한 soak, hit-rate, compile-time, toolchain 비교 항목만 추적한다.

## 1) Transport / RUDP

- [x] UDP/RUDP attach smoke 기준을 고정하고 보고서 경로를 남긴다.
  - same-network UDP attach:
    - `docker run --rm --network "knights-stack_knights-stack" ... ./build-linux/stack_loadgen --host gateway-1 --port 6000 --udp-port 7000 --scenario tools/loadgen/scenarios/udp_attach_login_only.json --report build/loadgen/udp_attach_login_only.trace.json --verbose`
    - 결과: `udp_bind_ok=4 udp_bind_fail=0 attach_failures=0`
  - same-network RUDP attach:
    - `docker run --rm --network "knights-stack_knights-stack" ... ./build-linux/stack_loadgen --host gateway-1 --port 6000 --udp-port 7000 --scenario tools/loadgen/scenarios/rudp_attach_login_only.json --report build/loadgen/rudp_attach_login_only.trace.json --verbose`
    - 결과: `rudp_attach_ok=4 rudp_attach_fallback=0`
  - Windows host-path direct same-gateway:
    - `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/udp_attach_login_only.json --report build/loadgen/udp_attach_login_only.host.json --verbose`
    - `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/rudp_attach_login_only.json --report build/loadgen/rudp_attach_login_only.host.json --verbose`
    - 결과: host-path도 동일하게 attach success 확인

- [x] mixed traffic(TCP+UDP) 장시간 soak 테스트를 실행하고 결과를 기록한다.
  - 기준:
    - duration
    - error rate
    - reconnect / fallback count
    - 핵심 latency / throughput 요약
  - direct same-gateway baseline sample:
    - scenario: `tools/loadgen/scenarios/mixed_direct_udp_soak.json`
    - command: `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_udp_soak.json --report build/loadgen/mixed_direct_udp_soak.host.json`
    - result: `loadgen_summary scenario=mixed_direct_udp_soak transports=tcp,udp sessions=24 connected=24 authenticated=24 joined=10 success=283 errors=0 attach_failures=0 udp_bind_ok=4 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=0 throughput_rps=4.48 p95_ms=12.06`
    - report: `build/loadgen/mixed_direct_udp_soak.host.json`
  - HAProxy long TCP control sample:
    - scenario: `tools/loadgen/scenarios/mixed_session_soak_long.json`
    - command: `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/mixed_session_soak_long.json --report build/loadgen/mixed_session_soak_long.json`
    - result: `loadgen_summary scenario=mixed_session_soak_long transports=tcp sessions=48 connected=48 authenticated=48 joined=24 success=639 errors=0 attach_failures=0 udp_bind_ok=0 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=0 throughput_rps=9.64 p95_ms=12.83`
    - report: `build/loadgen/mixed_session_soak_long.json`
  - direct same-gateway long UDP sample:
    - scenario: `tools/loadgen/scenarios/mixed_direct_udp_soak_long.json`
    - command: `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_udp_soak_long.json --report build/loadgen/mixed_direct_udp_soak_long.host.json`
    - result: `loadgen_summary scenario=mixed_direct_udp_soak_long transports=tcp,udp sessions=48 connected=48 authenticated=48 joined=20 success=1128 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=0 throughput_rps=8.93 p95_ms=12.26`
    - report: `build/loadgen/mixed_direct_udp_soak_long.host.json`
  - direct same-gateway long RUDP sample:
    - scenario: `tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json`
    - command: `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json --report build/loadgen/mixed_direct_rudp_soak_long.host.json`
    - result: `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1134 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=8 rudp_attach_fallback=0 throughput_rps=8.98 p95_ms=12.08`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.host.json`
  - direct same-gateway long RUDP fallback sample:
    - env: `docker/stack/.env.rudp-fallback.example`
    - command: `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json --report build/loadgen/mixed_direct_rudp_soak_long.fallback.host.json`
    - result: `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1130 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=8 throughput_rps=8.96 p95_ms=14.21`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.fallback.host.json`
  - direct same-gateway long RUDP OFF sample:
    - env: `docker/stack/.env.rudp-off.example`
    - command: `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json --report build/loadgen/mixed_direct_rudp_soak_long.off.host.json`
    - result: `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1131 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=0 rudp_attach_fallback=8 throughput_rps=8.96 p95_ms=12.95`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.off.host.json`
  - direct same-gateway long RUDP env restore proof:
    - env: `docker/stack/.env.rudp-attach.example`
    - command: `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json --report build/loadgen/mixed_direct_rudp_soak_long.final.matrix.host.json`
    - result: `loadgen_summary scenario=mixed_direct_rudp_soak_long transports=rudp,tcp sessions=48 connected=48 authenticated=48 joined=20 success=1131 errors=0 attach_failures=0 udp_bind_ok=8 udp_bind_fail=0 rudp_attach_ok=8 rudp_attach_fallback=0 throughput_rps=8.96 p95_ms=13.04`
    - report: `build/loadgen/mixed_direct_rudp_soak_long.final.matrix.host.json`
  - note:
    - current mixed proof uses TCP chat/ping/login_only + UDP login_only attach because UDP workload support is still attach-only.
    - current long RUDP mixed proof is a success-path sample; fallback/OFF proof는 `rudp_attach_login_only` 계열 env run으로 계속 분리해 추적한다.
    - `docker/stack/.env.rudp-*.example` 실행 전 `GATEWAY_UDP_BIND_SECRET`는 non-empty 값으로 교체해야 한다.

- [x] RUDP attach canary/fallback 결과를 기록한다.
  - 기준:
    - `rudp_attach_successes`
    - `rudp_attach_fallbacks`
    - gateway canary/allowlist 설정값
    - report path / summary line
  - 실행 시작점:
    - `tools/loadgen/scenarios/rudp_attach_login_only.json`
    - prerequisite: `GATEWAY_RUDP_ENABLE=1`, `GATEWAY_RUDP_CANARY_PERCENT=100`, non-empty `GATEWAY_RUDP_OPCODE_ALLOWLIST`
    - success-path smoke proof:
      - `build/loadgen/rudp_attach_login_only.trace.json`
      - `build/loadgen/rudp_attach_login_only.host.json`
      - 결과: `rudp_attach_ok=4 rudp_attach_fallback=0`
    - forced-fallback smoke proof:
      - `pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-fallback.example"`
      - `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/rudp_attach_login_only.json --report build/loadgen/rudp_attach_login_only.fallback.json --verbose`
      - 결과: `rudp_attach_ok=0 rudp_attach_fallback=4 errors=0 attach_failures=0`
    - RUDP OFF invariance proof:
      - `pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-off.example"`
      - `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/rudp_attach_login_only.json --report build/loadgen/rudp_attach_login_only.off.json --verbose`
      - 결과: `rudp_attach_ok=0 rudp_attach_fallback=4 errors=0 attach_failures=0`
    - env restore proof:
      - `pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"`
      - `build-windows\Release\stack_loadgen.exe --host 127.0.0.1 --port 36100 --udp-port 7000 --scenario tools/loadgen/scenarios/rudp_attach_login_only.json --report build/loadgen/rudp_attach_login_only.final.json`
      - 결과: `rudp_attach_ok=4 rudp_attach_fallback=0 errors=0 attach_failures=0`
    - 해석:
      - `rudp_attach_fallback`는 forced-fallback scenario에서 기대값이며 단독으로 error를 의미하지 않는다.
      - `GATEWAY_RUDP_ENABLE=0`에서도 attach failure 없이 fallback으로 수렴해야 OFF invariance가 성립한다.

- [x] 전체 loadgen 시스템 matrix를 재점검하고 결과를 남긴다.
  - attach env baseline + TCP control + direct same-gateway mixed samples + fallback/OFF + env restore를 모두 재실행했다.
  - baseline protocol checks:
    - `python tests/python/verify_chat.py`
    - `python tests/python/verify_pong.py`
  - matrix reports:
    - `build/loadgen/steady_chat.matrix.json`
    - `build/loadgen/mixed_session_soak.matrix.json`
    - `build/loadgen/mixed_session_soak_long.matrix.json`
    - `build/loadgen/udp_attach_login_only.matrix.host.json`
    - `build/loadgen/rudp_attach_login_only.matrix.host.json`
    - `build/loadgen/mixed_direct_udp_soak.matrix.host.json`
    - `build/loadgen/mixed_direct_udp_soak_long.matrix.host.json`
    - `build/loadgen/mixed_direct_rudp_soak_long.matrix.host.json`
    - `build/loadgen/rudp_attach_login_only.fallback.matrix.host.json`
    - `build/loadgen/mixed_direct_rudp_soak_long.fallback.matrix.host.json`
    - `build/loadgen/rudp_attach_login_only.off.matrix.host.json`
    - `build/loadgen/mixed_direct_rudp_soak_long.off.matrix.host.json`
    - `build/loadgen/rudp_attach_login_only.final.matrix.host.json`
    - `build/loadgen/mixed_direct_rudp_soak_long.final.matrix.host.json`
  - matrix summary:
    - all runs finished with `errors=0`
    - attach env: `rudp_attach_ok=4`, long mixed `rudp_attach_ok=8`
    - fallback/OFF env: long mixed `rudp_attach_fallback=8`, attach-only `rudp_attach_fallback=4`
    - final restore: attach-only `rudp_attach_ok=4`, long mixed `rudp_attach_ok=8`

## 2) CI Cache / Build Throughput

- [ ] `CI Prewarm`이 PR CI hit rate에 미치는 영향을 최소 1주간 측정한다.
  - 기준:
    - exact hit rate
    - restore elapsed time
    - main / PR 간 drift

- [ ] `Windows SCCache PoC`의 적용 전/후 compile 시간을 비교하고 채택/비채택을 결정한다.
  - 기준:
    - cold build time
    - warm build time
    - cache hit ratio
    - 운영 복잡도

## 3) Toolchain Strategy

- [ ] Conan2 binary remote 전략을 현재 캐시 방식과 비교하고 Go/No-Go 결론을 낸다.
  - 기준:
    - end-to-end CI wall clock
    - cache miss recovery time
    - 운영 복잡도
    - 재현성 / 장애 복구성

## Notes

- 실행 backlog / 운영 마감 항목은 `tasks/todo.md`에서 관리한다.
- 완료 이력과 장문 배경은 git history 및 archived task docs를 참고한다.
