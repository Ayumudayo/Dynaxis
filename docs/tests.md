# 테스트 가이드

이 문서는 Dynaxis 저장소의 현재 검증 entrypoint다.
세부 시나리오 카탈로그는 각 도구 문서가 source of truth를 가진다. 특히 loadgen transport proof shape는 `tools/loadgen/README.md`를 따른다.

`server_core` public package surface를 건드렸다면 이 문서를 `docs/core-api/overview.md`, `docs/core-api/quickstart.md`, `docs/core-api/compatibility-policy.md`, `docs/core-api/checklists.md`와 함께 읽는다. core API 문서는 public package story를 정의하고, 이 문서는 그 story를 repo-wide 검증 entrypoint에 연결한다.

## 기본 로컬 게이트

```powershell
pwsh scripts/build.ps1 -Config Release
ctest --preset windows-test --output-on-failure
python tools/gen_opcode_docs.py --check
python tools/check_markdown_links.py
```

`core/include/server/core/**`를 건드렸다면 아래를 추가한다.

```powershell
python tools/check_core_api_contracts.py --check-boundary
python tools/check_core_api_contracts.py --check-boundary-fixtures
python tools/check_core_api_contracts.py --check-stable-governance-fixtures
```

## 단위 및 계약 테스트

- 스토리지 기본 검증:
  - `pwsh scripts/build.ps1 -Config Debug -Target storage_basic_tests`
  - `build-windows/tests/Debug/storage_basic_tests.exe`
- 공개 API/계약 검증:
  - `core_public_api_smoke`
  - `core_public_api_headers_compile`
  - `core_public_api_stable_header_scenarios`
  - `core_public_api_fps_capability_smoke`
- public `server_core` package-first 검증:
  - `ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure`
  - `pwsh scripts/run_linux_installed_consumer.ps1`
- additional package extraction 관련 검증:
  - `ctest --test-dir build-windows -C Debug -R "FactoryPgInstalledPackageConsumer|FactoryRedisInstalledPackageConsumer" --output-on-failure`

## Stack / Integration Verification

- Docker stack:
  - `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
  - `pwsh scripts/deploy_docker.ps1 -Action down`
- Observability 포함:
  - `pwsh scripts/run_full_stack_observability.ps1`
- write-behind smoke:
  - `scripts/smoke_wb.ps1 -Config Debug -BuildDir build-windows`

## Python Proof Suites

- continuity / restart:
  - `python tests/python/verify_session_continuity.py`
  - `python tests/python/verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline`
  - `python tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance`
  - matrix stages:
  - `phase5-recovery-baseline`: default topology에서 gateway restart, server restart, locator fallback, world residency fallback, world owner fallback을 한 번에 검증한다
  - `default-closure`: default topology에서 admin control-plane, world drain progress, world drain migration closure, world migration handoff, app-local target-room migration handoff를 검증한다
  - `same-world-closure`: same-world topology에서 explicit owner transfer commit과 world drain transfer closure를 검증한다
  - `python tests/python/verify_session_continuity_restart.py --scenario gateway-restart`
  - `python tests/python/verify_session_continuity_restart.py --scenario server-restart`
  - `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-drain-progress`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-drain-transfer-closure`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-drain-migration-closure`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-owner-transfer-commit`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-migration-handoff`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-migration-target-room-handoff`
- FPS transport/runtime:
  - `python tests/python/verify_fps_state_transport.py`
  - `python tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`
  - acceptance stages:
  - `rudp-attach`: direct RUDP attach path
  - `udp-only-off`: rollout OFF path
  - `rollout-fallback`: canary/rollout fallback path
  - `protocol-fallback`: malformed/protocol impairment fallback path
  - `udp-quality-impairment`: deterministic loss/jitter/reorder/duplicate path
  - `rudp-restart`: restart/recovery path
  - `python tests/python/verify_fps_rudp_transport.py --scenario attach`
  - `python tests/python/verify_fps_rudp_transport.py --scenario off`
  - `python tests/python/verify_fps_rudp_transport.py --scenario rollout-fallback`
  - `python tests/python/verify_fps_rudp_transport.py --scenario protocol-fallback`
  - `python tests/python/verify_fps_rudp_transport.py --scenario udp-quality-impairment`
  - `python tests/python/verify_fps_rudp_transport.py --scenario restart`
  - `python tests/python/verify_fps_rudp_transport_matrix.py`
- admin/control-plane:
  - `python tests/python/verify_admin_api.py`
  - `python tests/python/verify_admin_auth.py`
  - `python tests/python/verify_admin_read_only.py`
  - current coverage includes world policy, named world drain progress/orchestration plus transfer/migration closure, named world owner transfer, world migration envelope, app-local migration payload kinds, and desired/observed/reconciliation/actuation plus actuation-request/status/execution/realization/adapter/runtime-assignment topology control-plane behavior
  - `verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance` is the preferred high-level entrypoint when both topology control-plane proof and runtime closure proof, including runtime-assignment live scale-out, are needed together
  - `verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline` is the preferred high-level entrypoint when restart/recovery evidence must be gathered as one repeatable sequence rather than as scattered single scenarios
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id>` is the preferred direct-path report entrypoint when Phase 5 FPS soak JSON reports must be captured under a named `run_id`
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening` is the preferred hardening entrypoint when long mixed soak plus FPS direct-path reruns are being collected for threshold decisions
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening --execution-mode hostnet-container` is the preferred scheduled/manual Linux hardening entrypoint when `.github/workflows/ci-hardening.yml` collects Phase 5 artifacts
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --capture-set rudp-success-only` is the preferred focused rerun entrypoint when only `mixed_direct_rudp_soak_long` success-path variance needs reconfirmation
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --capture-set rudp-off-only --execution-mode hostnet-container` is the preferred focused history entrypoint when hostnet RUDP OFF-path variance needs another sample without rerunning the full budget set
  - `python tests/python/verify_fps_netem_rehearsal.py --scenario fps-pair` is the preferred manual ops-only entrypoint when OS-level `tc netem` loss/jitter/reorder shaping must be rehearsed on the Linux stack path
  - validated manual rehearsal artifact:
    - `build/phase5-evidence/20260318-121332Z/netem/manifest.json`

## Load Generator

- canonical usage and scenario catalog:
  - `tools/loadgen/README.md`
- current supported proof families:
  - TCP chat/ping/login workloads
  - UDP attach, direct ping, FPS input proof
  - RUDP attach/fallback/OFF, direct ping, FPS input proof
- direct UDP/RUDP proofs require direct gateway TCP+UDP ports, not HAProxy frontend routing

## Main Merge Policy

- `main` is PR-only and protected; direct pushes are not the intended path.
- branch protection applies to admins and requires conversation resolution before merge.
- the only required status check is `windows-fast-tests` from `.github/workflows/ci.yml`.
- path-gated workflows stay non-required on purpose so unrelated PRs do not stall on checks that never dispatch.
- when a PR touches the matching surface, reviewers are expected to wait for the relevant path-gated workflow to pass before merging.

## CI Surfaces

- `.github/workflows/ci.yml`
  - always-on required gate
  - `windows-fast-tests`: Windows build/test + opcode/doc checks
- `.github/workflows/ci-api-governance.yml`
  - path-gated optional gate for `core/**`, core API docs, and contract fixtures
  - jobs: `core-api-consumer-windows`, `core-api-consumer-linux`
- `.github/workflows/ci-stack.yml`
  - path-gated optional gate for stack/runtime/integration surfaces
  - job: `linux-docker-stack`
  - includes `verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance --no-build`, `verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline --no-build`, and `verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance --no-build`
- `.github/workflows/ci-extensibility.yml`
  - path-gated optional gate for plugin/Lua/extensibility surfaces
  - job: `linux-extensibility`
- `.github/workflows/ci-hardening.yml`
  - scheduled/manual hardening gate; not a PR-required check
  - sanitizer/fuzz/perf hardening paths
  - collects `phase5-budget-evidence` artifacts through `capture_phase5_evidence.py --include-budget-hardening --execution-mode hostnet-container`
  - does not run OS-level `tc netem` rehearsal; that path stays manual/ops-only because it mutates live container qdisc state and is intentionally kept outside the accepted baseline gate
- `.github/workflows/ci-prewarm.yml`
  - scheduled/manual cache prewarm; not a merge gate
  - uploads telemetry artifacts for Windows Conan cache restore/save and Linux base-image prewarm elapsed time
  - current additional telemetry artifact set: `build/ci-prewarm-gh-run-23247349242/windows-conan-prewarm.json`, `build/ci-prewarm-gh-run-23247349242/linux-base-image-prewarm.json`
- `.github/workflows/windows-sccache-poc.yml`
  - workflow-dispatch-only compile-cache comparison path; not a merge gate
  - records same-run `without_sccache` vs `with_sccache` pass1/pass2 timings plus sccache stats
  - uploads `build/windows-sccache-poc/windows-sccache-poc.json` and raw `sccache-pass*.stats.txt` artifacts
  - current first captured comparison artifact: `build/windows-sccache-poc-gh-run-23245866965/windows-sccache-poc-23245866965/windows-sccache-poc.json`
- `.github/workflows/conan2-poc.yml`
  - workflow-dispatch-only Conan current-cache strategy probe; not a merge gate
  - uploads `build/conan-strategy-poc/windows-conan-current-cache.json`
  - current framed baseline artifact: `build/conan-strategy-poc-gh-run-23246958472/conan-strategy-poc-windows-23246958472/windows-conan-current-cache.json`
