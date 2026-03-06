# Active TODO

> Status (2026-03-07): Active backlog only. Completed execution history was intentionally pruned.
> Historical detail: use git history plus archived task docs under `tasks/`.

## 1) Transport / RUDP

- [ ] RUDP 운영 검증을 마감한다.
  - OFF 불변성
  - ON canary fallback
  - 운영/롤백 리허설 근거
- [ ] NAT rebinding / MTU / 메모리 / 보안 강화 항목을 설계 문서 + 후속 이슈로 분리해 추적한다.

## 2) CI / Repo Ops

- [ ] `main` branch protection과 required check 정책을 정리한다.
  - 기본 required: `CI`
  - path-gated workflow는 optional 유지 또는 별도 no-op wrapper 설계 후 required 승격
- [ ] 저장소 문서/노트에 로컬 절대 경로가 남아 있지 않도록 전수 점검하고 정리한다.
  - 대상: workspace-local absolute path 흔적
  - 기준: 저장소 상대 경로 또는 일반 markdown 링크만 허용

## 3) Load Generator (TCP Phase)

- [x] TCP load generator 요구사항/범위를 문서로 고정한다.
  - 대상: 기존 `haproxy -> gateway_app -> server_app`
  - 비대상: 별도 synthetic server 추가
- [x] `tools/loadgen` 스캐폴드를 추가한다.
  - headless CLI
  - 시나리오/리포트 출력 경로
- [x] 기존 프로토콜 자산을 재사용해 TCP 클라이언트 시뮬레이션을 구현한다.
  - login
  - join
  - chat / ping
- [x] 최소 시나리오 2개를 제공한다.
  - steady chat
  - mixed session soak
- [x] 결과 요약을 JSON 또는 동등한 기계 판독 형식으로 출력한다.
  - success/error count
  - latency summary
  - throughput
- [x] Docker stack 기준 실행/검증 절차를 문서화한다.

### Review

- `pwsh scripts/build.ps1 -Config Release -Target stack_loadgen`
  - `stack_loadgen.exe` Release 빌드 성공
- `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
  - Docker stack + HAProxy frontend 기동 성공
- `python tests/python/verify_chat.py`
  - PASS
- `python tests/python/verify_pong.py`
  - PASS
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/steady_chat.json --report build/loadgen/steady_chat.json`
  - `loadgen_summary scenario=steady_chat transports=tcp sessions=24 connected=24 authenticated=24 joined=24 success=155 errors=0 throughput_rps=8.60 p95_ms=14.41`
- `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/mixed_session_soak.json --report build/loadgen/mixed_session_soak.json`
  - `loadgen_summary scenario=mixed_session_soak transports=tcp sessions=24 connected=24 authenticated=24 joined=12 success=85 errors=0 throughput_rps=4.67 p95_ms=12.66`
- 참고
  - fresh stack 기준 단일 loadgen binary + TCP transport 검증 완료
  - repeated same-stack run 회귀는 아래 Gateway Follow-up에서 별도로 마감

## 4) Gateway Follow-up

- [x] repeated loadgen run에서 드러난 `gateway_app` 종료 경로 결함을 수정한다.
  - 재현: back-to-back loadgen run
  - 증상: `free(): double free detected in tcache 2`
  - 범위: `GatewayApp::BackendConnection` close/write/read 수명 관리

### Review

- `pwsh scripts/build.ps1 -Config Release -Target gateway_app`
  - gateway 수정 반영 후 Release 빌드 성공
- `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
  - 수정 반영 Docker image 재빌드 성공
- same-stack repeated run
  - `python tests/python/verify_chat.py` PASS
  - `steady_chat` 1회차: `connected=24 authenticated=24 joined=24 success=152 errors=0 throughput_rps=8.44 p95_ms=16.13`
  - `mixed_session_soak` 1회차: `connected=24 authenticated=24 joined=12 success=81 errors=0 throughput_rps=4.44 p95_ms=15.67`
  - `steady_chat` 2회차: `connected=24 authenticated=24 joined=24 success=153 errors=0 throughput_rps=8.50 p95_ms=14.37`
  - `mixed_session_soak` 2회차: `connected=24 authenticated=24 joined=12 success=84 errors=0 throughput_rps=4.60 p95_ms=14.06`
- `python tests/python/verify_pong.py`
  - repeated run 이후에도 PASS
- container health
  - `gateway-1`, `gateway-2` 컨테이너가 same-stack repeated run 이후에도 `Up` 유지
  - gateway logs에 `free(): double free detected in tcache 2` 재발 없음

## 5) Pre-Push CI Verification

- [x] `CI` 로컬 동등 검증을 수행한다.
  - opcode / wire / doxygen / workflow YAML check
  - Windows Release build
  - `ctest --preset windows-test`
- [x] `CI API Governance` 로컬 동등 검증을 수행한다.
  - core boundary / fixture / stable governance checks
  - core public API consumer targets + tests
- [x] `CI Stack` 로컬 동등 검증을 수행한다.
  - runtime off baseline smoke
  - runtime on stack smoke
- [x] `CI Extensibility` 로컬 동등 검증을 수행한다.
  - runtime toggle metrics
  - plugin / script smoke

### Review

- static checks
  - `python tools/gen_opcode_docs.py --check`
  - `python tools/check_doxygen_coverage.py`
  - `python tests/python/test_check_doxygen_coverage.py`
  - `python tools/gen_wire_codec.py protocol/wire_map.json core/include/server/wire/codec.hpp`
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`
  - 결과: 모두 통과
- Windows build / test
  - `pwsh scripts/build.ps1 -Config Release`
  - `pwsh scripts/build.ps1 -Config Release -Target core_public_api_smoke`
  - `pwsh scripts/build.ps1 -Config Release -Target core_public_api_headers_compile`
  - `pwsh scripts/build.ps1 -Config Release -Target core_public_api_stable_header_scenarios`
  - `ctest --preset windows-test --parallel 8 --output-on-failure`
  - 결과: `239/239 pass` (`9 skipped`)
  - 참고: 첫 `Release` 전체 빌드는 `server_plugin_chain_v2_tests` GoogleTest discovery timeout으로 한 번 실패했지만, 바이너리 자체는 정상적으로 `--gtest_list_tests`를 반환했고 즉시 재실행에서 통과했다.
- core API governance follow-up
  - `core/include/server/core/net/connection.hpp` 변경으로 stable header governance가 걸려 `docs/core-api/net.md`, `docs/core-api/changelog.md`, `docs/core-api/compatibility-matrix.json`, `core/include/server/core/api/version.hpp`를 함께 갱신했다.
  - 임시 index/tree + synthetic PR event payload 기준
    - `python tools/check_core_api_contracts.py --check-doc-freshness ...`
    - `python tools/check_core_api_contracts.py --check-stable-change-governance ...`
    - `python tools/check_core_api_contracts.py --check-pr-governance ...`
  - 결과: 모두 통과
- Docker stack
  - baseline off
    - `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
    - `python tests/python/verify_runtime_toggle_metrics.py --expect-chat-hook-enabled 0 --expect-lua-enabled 0`
    - `python tests/python/verify_pong.py`
    - `python tests/python/verify_chat.py`
  - runtime on
    - `python tests/python/verify_runtime_toggle_metrics.py --expect-chat-hook-enabled 1 --expect-lua-enabled 1`
    - `python tests/python/test_load_balancing.py`
    - `python tests/python/verify_whisper_cross_instance.py`
    - `python tests/python/verify_admin_api.py`
    - `python tests/python/verify_admin_auth.py`
    - `python tests/python/verify_admin_control_plane_e2e.py`
  - 결과: 모두 통과
- Docker extensibility
  - `python tests/python/verify_plugin_hot_reload.py --check-only`
  - `python tests/python/verify_plugin_hot_reload.py`
  - `python tests/python/verify_plugin_v2_fallback.py`
  - `python tests/python/verify_plugin_rollback.py`
  - `python tests/python/verify_script_hot_reload.py`
  - `python tests/python/verify_script_fallback_switch.py`
  - `python tests/python/verify_chat_hook_behavior.py`
  - 결과: 모두 통과

## 6) Unified Load Generator

- [x] 장기 과제로 richer scenario language 검토 항목을 남긴다.
  - 현재는 JSON schema 유지
  - 분기 / 반복 / 장애 주입 / 복합 transport 제어가 커지면 별도 scenario language 평가
- [x] 다음 세션 handoff용 next-steps 문서를 유지한다.
  - 문서: `docs/tests/loadgen-next-steps.md`
  - 목적: UDP/RUDP transport phase와 scenario/schema hardening 순서를 세션 간에 이어받기 쉽게 유지
- [x] 단일 loadgen binary 기준의 1차 통합 계획을 문서/체크리스트로 고정한다.
  - 목적: transport별 별도 프로그램 추가 대신 공통 harness 유지
  - 비범위: 이번 턴에 UDP/RUDP 구현까지 확장하지 않음
- [x] 현재 TCP 전용 구현을 transport-aware 구조로 재편한다.
  - 시나리오에 `transport` 필드를 도입
  - TCP 구현은 adapter/driver 계층으로 내린다
- [x] 실행 타깃과 문서를 단일 loadgen 기준으로 정리한다.
  - build/run/readme/plan 문서
  - 기존 `tcp_loadgen` 언급 정리
- [x] 로컬 검증으로 회귀가 없는지 확인한다.
  - Release build
  - 기존 steady/mixed scenario 실행

### Review

- build / compatibility
  - `pwsh scripts/build.ps1 -Config Release -Target stack_loadgen`
  - `pwsh scripts/build.ps1 -Config Release -Target tcp_loadgen`
  - 결과: 새 실행 파일 `stack_loadgen.exe` 빌드 성공, 기존 `tcp_loadgen` build target도 compatibility alias로 유지
- unified harness refactor
  - `SessionClient`를 interface로 올리고 `TcpSessionClient` 구현 + `make_session_client(...)` factory를 추가했다.
  - 시나리오 `groups[]`에 `transport` 필드를 도입했고 현재 샘플은 모두 `tcp`를 명시한다.
  - report/summary에 `transports=tcp`를 노출한다.
- docs
  - `docs/tests/loadgen-plan.md`
  - `docs/tests/loadgen-next-steps.md`
  - `docs/tests.md`
  - `tools/loadgen/README.md`
  - 결과: 단일 `stack_loadgen` 기준으로 문서 정리 완료
- handoff intent
  - `docs/tests/loadgen-next-steps.md`에 Phase A/B/C, 추천 파일 분해, 검증 전략, known pitfalls, 시작 명령을 기록했다.
  - 다른 세션이 “무엇을 먼저 하고 무엇을 미루는지” 바로 이어받을 수 있는 수준으로 정리했다.
- runtime validation
  - `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/steady_chat.json --report build/loadgen/steady_chat.json`
  - `build-windows\\Release\\stack_loadgen.exe --host 127.0.0.1 --port 6000 --scenario tools/loadgen/scenarios/mixed_session_soak.json --report build/loadgen/mixed_session_soak.json`
  - 결과:
    - `steady_chat`: `loadgen_summary scenario=steady_chat transports=tcp sessions=24 connected=24 authenticated=24 joined=24 success=155 errors=0 throughput_rps=8.60 p95_ms=14.41`
    - `mixed_session_soak`: `loadgen_summary scenario=mixed_session_soak transports=tcp sessions=24 connected=24 authenticated=24 joined=12 success=85 errors=0 throughput_rps=4.67 p95_ms=12.66`
- unsupported transport guardrail
  - temporary scenario에서 `transport=udp`로 실행
  - 결과: `loadgen error: transport 'udp' is not implemented yet`

## Quantitative Validation

- 정량적 검증 backlog는 `tasks/quantitative-validation.md`에서 별도 추적한다.

## Archived References

- `tasks.md`
  - Archived snapshot
- `tasks/next-plan.md`
  - Archived
- `tasks/core-engine-full-todo.md`
  - Archived
- `tasks/quantitative-validation.md`
  - Active measurement / benchmark backlog
