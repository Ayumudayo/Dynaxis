# 테스트 가이드

## 1. 스토리지 단위 테스트
- 대상: `storage_basic_tests` (Postgres + Repository 기본 검증)
- 준비
  - 환경 변수에 `DB_URI` 지정
  - `tools/migrations/*.sql` 또는 `migrations_runner`로 스키마 적용
- 실행 (PowerShell)
  ```
  scripts/build.ps1 -Config Debug -BuildDir build-windows -Target storage_basic_tests
  build-windows/tests/Debug/storage_basic_tests.exe
  ```
- DB 연결이 없으면 테스트는 자동 skip 된다.

## 2. Redis/Write-behind 스모크 테스트
`wb_worker`, `wb_emit`, `wb_check`를 묶어 Streams→DB 라운드트립을 검증한다.
```
scripts/smoke_wb.ps1 -Config Debug -BuildDir build-windows
```
- Redis/DB URI를 환경 변수로 지정하고 `WRITE_BEHIND_ENABLED=1` 상태에서 실행.
- 완료 후 `wb_check`가 `session_events` 테이블을 조회해 XADD→DB 반영 여부를 출력한다.

## 3. E2E (Gateway + Server + Client)
- 준비: Redis(필수) / Postgres(옵션)을 준비하고, 필요한 환경 변수를 설정한다. (`.env.example` 참고)
- 실행 (Docker stack, HAProxy 포함)
  - 권장(Windows/WSL/Linux): `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
  - Observability 포함: `pwsh scripts/run_full_stack_observability.ps1`
- 검증 순서
  1. 클라이언트에서 로그인/룸 입장/채팅/퇴장 수행 (HAProxy: `127.0.0.1:6000`)
  2. `wb_flush`, `wb_pending` 로그 모니터링
  3. Postgres `session_events` 확인: `select id,event_id,type,ts,user_id,session_id,room_id from session_events order by id desc limit 20;`

### 3.1 Session Continuity Proof

- continuity lease/runtime을 검증하려면 `SESSION_CONTINUITY_ENABLED=1` 상태로 stack을 띄운다.
- 기본 continuity proof:
  - `python tests/python/verify_session_continuity.py`
- restart rehearsal proof:
  - `python tests/python/verify_session_continuity_restart.py --scenario gateway-restart`
  - `python tests/python/verify_session_continuity_restart.py --scenario server-restart`
- locator fallback proof:
  - `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- world residency fallback proof:
  - `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
- restart/locator rehearsal은 live stack을 직접 restart하거나 sticky keys를 지우므로 서로 병렬 실행하지 말고 순차 실행한다.
- gateway proof는 surviving gateway를 통한 resume alias routing hit를 확인한다.
- server proof는 restarted backend 이후에도 logical session / room continuity가 유지되는지 확인한다.
- locator fallback proof는 exact resume alias binding을 제거한 뒤에도 same shard locator hint로 resume가 복원되는지 확인한다.
- world residency fallback proof는 persisted world key를 제거한 뒤 `default world + lobby` safe fallback이 적용되는지 확인한다.

## 4. Redis 통합 테스트(권장)
- Docker Compose로 Redis/DB를 띄우고 `tests/redis_integration_tests`(추가 예정)를 작성해 Presence/Room membership 캐시를 검증한다.
- 주요 시나리오: Presence TTL 만료, Sticky Session 재바인딩, Streams pending 복구.

## 5. Observability 체크
- `.env`의 `METRICS_PORT`를 지정한 뒤 아래로 확인:
  - `curl http://127.0.0.1:<port>/metrics`
  - `curl http://127.0.0.1:<port>/healthz`
  - `curl http://127.0.0.1:<port>/readyz`
- 핵심 지표(현재 구현 기준):
  - server_app: `runtime_build_info`, `chat_session_active`, `chat_dispatch_latency_ms_*`, `chat_dispatch_opcode_named_total`, `chat_job_queue_depth`, `chat_subscribe_total`, `chat_subscribe_last_lag_ms`, `chat_hook_plugins_enabled`
  - wb_worker: `wb_pending`, `wb_flush_total`, `wb_flush_batch_size_last`, `wb_flush_commit_ms_last`

## 6. CI 권장 플랜
- 기본 required gate: `.github/workflows/ci.yml`
  - Windows: Release 빌드 + `ctest --preset windows-test`
  - Opcodes / wire / Doxygen: 코드젠/문서 최신 상태 강제
- Stack smoke: `.github/workflows/ci-stack.yml`
  - Linux: Docker stack baseline/off + runtime-on 기본 스모크
- Extensibility smoke: `.github/workflows/ci-extensibility.yml`
  - Linux: plugin/script 메트릭, 핫리로드, fallback/rollback 검증
- Hardening: `.github/workflows/ci-hardening.yml`
  - Linux: `linux-asan`(ASan/UBSan), fuzz, soak/perf 검증

## 7. Push 전 로컬 검증 게이트
- 큰 변경(빌드/의존성/워크플로우/다중 모듈 변경)은 push 전에 로컬 검증을 먼저 통과한다.
- 권장 기본 게이트:
  - `pwsh scripts/build.ps1 -Config Release`
  - `ctest --preset windows-test --output-on-failure`
- `core/include/server/core/**` 변경 시 추가 게이트:
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`
- package-first extraction 관련 변경 시 추가 게이트:
  - `ctest -C Debug --test-dir build-windows/tests -R "CoreInstalledPackageConsumer|FactoryPgInstalledPackageConsumer|FactoryRedisInstalledPackageConsumer" --output-on-failure`
- publish automation 관련 변경 시 추가 게이트:
  - `pwsh scripts/publish_factory_packages.ps1 -BuildDir build-windows -OutputRoot artifacts/factory-packages -Zip -CleanOutput`
  - artifact prefix를 대상으로 package consumer configure/build 재확인
- Linux 경로는 Docker stack 로컬 검증 + 원격 GitHub CI 결과를 기준으로 판단한다.

## 8. Loadgen Plan

- 설계 문서: [loadgen-plan.md](tests/loadgen-plan.md)
- 다음 세션 handoff: [loadgen-next-steps.md](tests/loadgen-next-steps.md)
- 실행 가이드: [README.md](../tools/loadgen/README.md)
- 목적: 기존 `haproxy -> gateway_app -> server_app` 경로를 대상으로 단일 loadgen harness로 soak/latency/throughput을 재현 가능하게 측정
- 현재 지원:
  - `tcp` workload (`chat` / `ping` / `login_only`)
  - `udp` attach validation (`login_only` only)
  - `rudp` attach/fallback visibility (`login_only` only)
  - basic scenario catalog: `steady_chat`, `mixed_session_soak`, `mixed_session_soak_long`, `mixed_direct_udp_soak`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long`, `udp_attach_login_only`, `rudp_attach_login_only`
- attach 검증 주의:
  - UDP/RUDP attach는 gateway-local bind ticket을 사용하므로 HAProxy가 아니라 같은 gateway의 TCP+UDP 포트를 직접 지정해야 한다.
  - `docker/stack/.env.rudp-*.example`를 쓸 때는 `GATEWAY_UDP_BIND_SECRET=replace-with-non-empty-secret`를 실제 non-empty 값으로 바꿔야 한다.
  - 2026-03-07 기준 same-network / Windows host-path direct same-gateway 검증에서 `udp_bind_ok=4` 및 `rudp_attach_ok=4`를 확인했다.
  - mixed direct same-gateway quantitative sample도 확보했다: `build/loadgen/mixed_direct_udp_soak.host.json`에서 `success=283 errors=0 udp_bind_ok=4 udp_bind_fail=0`.
  - long control/mixed sample도 확보했다: `build/loadgen/mixed_session_soak_long.json`, `build/loadgen/mixed_direct_udp_soak_long.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.host.json`.
  - long mixed RUDP policy comparison도 확보했다: `build/loadgen/mixed_direct_rudp_soak_long.fallback.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.off.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.final.matrix.host.json`.
  - forced fallback proof도 확보했다: `build/loadgen/rudp_attach_login_only.fallback.json`에서 `rudp_attach_ok=0 rudp_attach_fallback=4 errors=0`.
  - OFF invariance proof도 확보했다: `build/loadgen/rudp_attach_login_only.off.json`에서 `rudp_attach_ok=0 rudp_attach_fallback=4 errors=0`.
  - 테스트 후 normal attach env restore proof도 남겼다: `build/loadgen/rudp_attach_login_only.final.json`에서 `rudp_attach_ok=4 rudp_attach_fallback=0 errors=0`.
  - `rudp_attach_fallback`는 fallback visibility counter이며 forced-fallback scenario에서는 기대값이다.
  - 현재 attach 검증은 문서화된 로컬/수동 검증 게이트이며 `.github/workflows/`의 required CI에는 아직 포함되지 않는다.
