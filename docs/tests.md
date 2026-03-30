# 테스트 가이드

이 문서는 Dynaxis 저장소의 현재 검증 진입점이다.
세부 시나리오 카탈로그는 각 도구 문서가 기준 문서를 가진다. 특히 loadgen transport proof 형태는 `tools/loadgen/README.md`를 따른다.

`server_core` public package surface를 건드렸다면 이 문서를 `docs/core-api/overview.md`, `docs/core-api/quickstart.md`, `docs/core-api/compatibility-policy.md`, `docs/core-api/checklists.md`와 함께 읽는다. core API 문서는 public package story를 정의하고, 이 문서는 그 story를 저장소 전체 검증 진입점에 연결한다.

테스트 build ownership은 domain manifest로 나뉜다.
- prelude/dispatch: `tests/CMakeLists.txt`
- core targets: `tests/core/CMakeLists.txt`
- gateway targets: `tests/gateway/CMakeLists.txt`
- server targets: `tests/server/CMakeLists.txt`
- contract/policy lanes: `tests/contracts/CMakeLists.txt`, `tests/policy/CMakeLists.txt`

## 기본 로컬 게이트

```powershell
pwsh scripts/build.ps1 -Config Release
ctest --preset windows-test -R WindowsReleaseTreeReady --output-on-failure --no-tests=error
ctest --preset windows-test --parallel 8 --output-on-failure
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
  - `core_public_api_extensibility_smoke`
  - `core_public_api_realtime_capability_smoke`
  - `core_public_api_worlds_aws_smoke`
- public `server_core` package-first 검증:
  - `ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure`
  - `CoreInstalledPackageConsumer`는 `server_core_installed_consumer`와 `server_core_extensibility_consumer` 둘 다 실행한다
  - `pwsh scripts/run_linux_installed_consumer.ps1`
- additional package extraction 관련 검증:
  - `ctest --test-dir build-windows -C Debug -R "FactoryPgInstalledPackageConsumer|FactoryRedisInstalledPackageConsumer" --output-on-failure`
  - Redis consumer는 canonical `infra_redis_factory`를 우선 찾고, install prefix에 legacy compatibility package만 있을 때만 `server_storage_redis_factory`로 fallback한다

## 스택 / 통합 검증

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
- Realtime transport/runtime (current FPS workload proof):
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
- Kubernetes local/dev worlds harness:
  - `python tests/python/verify_worlds_kubernetes_localdev.py`
  - optional kubectl client validation:
    - `python tests/python/verify_worlds_kubernetes_localdev.py --kubectl-validate`
    - if `kubectl` is installed but no current context or reachable API server exists, the script reports a skip and still passes the deterministic manifest proof
  - generator/wrapper:
    - `python scripts/generate_k8s_topology.py --topology-config docker/stack/topologies/default.json`
    - `pwsh scripts/run_k8s_localdev_worlds.ps1 -KubectlValidate`
  - optional live `kind` runner/proof:
    - `pwsh scripts/run_k8s_localdev_kind.ps1 -Action up`
    - `python tests/python/verify_worlds_kubernetes_kind.py`
    - skips cleanly when `docker`, `kubectl`, `kind`, or the local `dynaxis-*:local` images are unavailable
  - optional live `kind` control-plane proof:
    - `python tests/python/verify_worlds_kubernetes_kind_control_plane.py`
    - proves one desired-topology plus runtime-assignment roundtrip through `admin-app` over a temporary `kubectl port-forward`
  - optional live `kind` world-closure proof:
    - `python tests/python/verify_worlds_kubernetes_kind_closure.py`
    - proves cross-world drain-to-migration closure on the default topology and same-world drain-to-transfer closure on the same-world topology
  - optional live `kind` continuity proof:
    - `python tests/python/verify_worlds_kubernetes_kind_continuity.py`
    - proves client-observable resume on live clusters for both cross-world migration and same-world transfer commit
  - optional live `kind` multi-gateway proof:
    - `python tests/python/verify_worlds_kubernetes_kind_multigateway.py`
    - proves `gateway-1` login plus `gateway-2` resume for both migration and transfer-commit flows
  - optional live `kind` restart proof:
    - `python tests/python/verify_worlds_kubernetes_kind_restart.py`
    - proves gateway rollout restart and source server StatefulSet rollout restart while continuity resume remains available
  - optional live `kind` locator-fallback proof:
    - `python tests/python/verify_worlds_kubernetes_kind_locator_fallback.py`
    - proves exact resume alias loss still recovers through the locator hint plus selector hit/fallback path on live clusters
  - optional live `kind` world-state fallback proof:
    - `python tests/python/verify_worlds_kubernetes_kind_world_state_fallback.py`
    - proves missing persisted world/world-owner state still falls back safely on the original backend and records explicit reason metrics
  - optional live `kind` Redis dependency outage proof:
    - `python tests/python/verify_worlds_kubernetes_kind_redis_outage.py`
    - proves `admin_app` detects Redis loss, registry-backed endpoints return `503`, `/readyz` stays green, and observed topology recovers after Redis returns
  - optional live `kind` worker metrics outage proof:
    - `python tests/python/verify_worlds_kubernetes_kind_worker_outage.py`
    - proves `admin_app` detects `wb_worker` metrics-path loss, `/api/v1/worker/write-behind` returns `503`, `/readyz` stays green, and the worker snapshot recovers after the path returns
  - optional live `kind` gateway ingress impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_gateway_ingress_impairment.py`
    - proves required Redis loss makes gateway readiness drop and fresh ingress reject while one existing bridged session still exchanges room traffic
  - optional live `kind` resume impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_resume_impairment.py`
    - proves required Redis loss blocks cross-gateway continuity resume for a disconnected session, then restores the same logical session with usable post-recovery traffic after recovery
  - optional live `kind` multi-fault impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_multifault_impairment.py`
    - proves required Redis loss plus `gateway-2` pod churn block resume until both recover, then restores the same logical session afterward
  - optional live `kind` metrics-budget proof:
    - `python tests/python/verify_worlds_kubernetes_kind_metrics_budget.py`
    - proves the current migration and transfer continuity stories also emit the expected gateway/server metrics without unexpected fallback growth
  - provider-path proof:
    - `build-windows/tests/Release/core_public_api_worlds_aws_smoke.exe`
    - proves one deterministic desired-topology -> Kubernetes pool binding -> AWS provider binding -> provider adapter status path through the public stable headers
  - preferred manual/release evidence runner:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id>`
    - writes `build/k8s-kind-evidence/<run_id>/manifest.json` plus one log per proof
  - focused fallback capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set fallback-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-locator-fallback/locator-fallback.json`
  - focused world-state capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set world-state-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-world-state-fallback/world-state-fallback.json`
  - focused outage capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set outage-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-redis-outage/redis-outage.json`
  - focused worker-outage capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set worker-outage-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-worker-outage/worker-outage.json`
  - focused impairment capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set impairment-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-gateway-ingress-impairment/gateway-ingress-impairment.json`
  - focused resume-impairment capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set resume-impairment-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-resume-impairment/resume-impairment.json`
  - focused multifault capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set multifault-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-multifault-impairment/multifault-impairment.json`
  - focused metrics capture:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set metrics-only`
    - writes `build/k8s-kind-evidence/<run_id>/kind-metrics-budget/metrics-budget.json`

## 부하 생성기(Load Generator)

- canonical usage and scenario catalog:
  - `tools/loadgen/README.md`
- current supported proof families:
  - TCP chat/ping/login workloads
  - UDP attach, direct ping, FPS input proof
  - RUDP attach/fallback/OFF, direct ping, FPS input proof
- direct UDP/RUDP proofs require direct gateway TCP+UDP ports, not HAProxy frontend routing

## main 병합 정책

- `main` is PR-only and protected; direct pushes are not the intended path.
- branch protection applies to admins and requires conversation resolution before merge.
- `main` branch protection의 required status check는 `Windows Build, Docs, and Tests`다.
- merge queue에서는 Baseline Checks와 path-gated integration lane만 계속 돈다. 즉 `Baseline Checks`, `Stack Integration`, `Extensibility Integration`은 merge queue 대상이고, `Core API Checks`, `Reliability Checks`, `Cache Prep`, `Conan Cache Strategy Probe`, `Compiler Cache Timing Probe`, `Factory Package Release`는 merge queue 대상이 아니다.
- path-gated lane은 여전히 non-required로 두어, 매칭되지 않는 변경이 dispatch되지 않았다는 이유로 PR이 불필요하게 대기하지 않게 한다.
- merge-queue lane 설정은 저장소 파일 밖의 GitHub 설정에서 관리한다.
- PR이 해당 표면을 건드리면, 리뷰어는 매칭되는 path-gated lane이 통과할 때까지 머지하지 않는 현재 운영 규칙을 유지한다.

## CI 검증 레인

### Validation Lanes

- `Baseline Checks` (`.github/workflows/ci.yml`)
  - 항상 도는 기본 검증 레인이다.
  - required status check 이름은 `Windows Build, Docs, and Tests`다.
  - Windows build/test + opcode/doc checks를 묶는 기본 게이트다.
  - generated protocol/wire header는 source tree가 아니라 CI 임시 generated tree로 재생성하고, tracked forwarder header drift가 없는지만 검사한다.
- `Core API Checks` (`.github/workflows/ci-api-governance.yml`)
  - `core/**`, core API docs, contract fixture를 위한 path-gated validation lane이다.
  - jobs: `Core API Governance and Consumer Validation (Windows)`, `Core API Governance and Consumer Validation (Linux)`
- `Stack Integration` (`.github/workflows/ci-stack.yml`)
  - stack/runtime/integration 표면을 위한 path-gated integration lane이다.
  - jobs: `Stack Runtime Integration (Linux)`, `Admin Read-Only Check (Linux)`, `Write-Behind Integration Check (Linux)`
  - `Stack Runtime Integration (Linux)`은 `verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance --no-build`, `verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline --no-build`, `verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance --no-build`를 포함한다.
  - `Admin Read-Only Check (Linux)`은 `verify_admin_read_only.py`로 admin read-only 제약을 검증한다.
  - `Write-Behind Integration Check (Linux)`은 `scripts/smoke_wb.sh`로 Redis Streams -> Postgres write-behind 적재 경로를 검증한다.
- `Extensibility Integration` (`.github/workflows/ci-extensibility.yml`)
  - plugin/Lua/extensibility 표면을 위한 path-gated integration lane이다.
  - job: `Plugin and Lua Runtime Integration (Linux)`
- `Reliability Checks` (`.github/workflows/ci-hardening.yml`)
  - scheduled/manual hardening lane이며 PR required check나 merge-queue lane으로 취급하지 않는다.
  - sanitizer/fuzz/perf hardening 경로를 돈다.
  - `capture_phase5_evidence.py --include-budget-hardening --execution-mode hostnet-container`를 통해 `phase5-budget-evidence` artifact를 수집한다.
  - OS-level `tc netem` rehearsal은 live container qdisc state를 바꾸므로 계속 manual ops 전용으로 남기고 baseline gate에는 넣지 않는다.

### Operational Support Lanes

- `Cache Prep` (`.github/workflows/ci-prewarm.yml`)
  - scheduled/manual cache prewarm lane이며 merge gate가 아니다.
  - Windows Conan cache restore/save와 Linux base-image prewarm elapsed time telemetry artifact를 업로드한다.
  - 현재 추가 telemetry artifact 예시는 `build/ci-prewarm-gh-run-23247349242/windows-conan-prewarm.json`, `build/ci-prewarm-gh-run-23247349242/linux-base-image-prewarm.json`이다.

### Probe Lanes

- `Conan Cache Strategy Probe` (`.github/workflows/conan2-poc.yml`)
  - workflow-dispatch-only Conan cache strategy probe이며 merge gate가 아니다.
  - `build/conan-strategy-poc/windows-conan-current-cache.json`을 업로드한다.
  - 현재 baseline artifact 예시는 `build/conan-strategy-poc-gh-run-23246958472/conan-strategy-poc-windows-23246958472/windows-conan-current-cache.json`이다.
- `Compiler Cache Timing Probe` (`.github/workflows/windows-sccache-poc.yml`)
  - workflow-dispatch-only compile cache timing probe이며 merge gate가 아니다.
  - 같은 run에서 `without_sccache`와 `with_sccache`의 pass1/pass2 timing, sccache stats를 비교한다.
  - `build/windows-sccache-poc/windows-sccache-poc.json`과 raw `sccache-pass*.stats.txt` artifact를 업로드한다.
  - 현재 첫 비교 artifact 예시는 `build/windows-sccache-poc-gh-run-23245866965/windows-sccache-poc-23245866965/windows-sccache-poc.json`이다.

### Release Lanes

- `Factory Package Release` (`.github/workflows/factory-package-publish.yml`)
  - workflow-dispatch-only release lane이며 PR required check나 merge-queue lane이 아니다.
  - job: `Factory Package Build and Validation (Windows)`
  - Windows factory package bundle을 빌드하고 `CoreInstalledPackageConsumer|FactoryPgInstalledPackageConsumer|FactoryRedisInstalledPackageConsumer` 검증 후 artifact를 업로드한다.
  - Redis package의 canonical export는 `infra_redis_factory`이고, `server_storage_redis_factory`는 compatibility alias/config package로 함께 검증된다.
  - release artifact 이름은 `dynaxis-factory-packages-windows-release`다.

## Kubernetes worlds harness 체크
- `KubernetesWorldsLocalDevManifestCheck`
  - local/ctest manifest proof for the topology-driven Kubernetes worlds harness
  - validates generated manifests and summary artifacts without requiring a live cluster
- `KubernetesWorldsKindLiveCheck`
  - optional live `kind` ctest for the topology-driven Kubernetes worlds harness
  - applies the generated manifest to an ephemeral `kind` cluster and skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindControlPlaneCheck`
  - optional live `kind` control-plane ctest for the topology-driven Kubernetes worlds harness
  - port-forwards `admin-app` and proves one topology actuation/runtime-assignment roundtrip; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindClosureCheck`
  - optional live `kind` closure ctest for the topology-driven Kubernetes worlds harness
  - proves named drain closure through migration and transfer orchestration on live clusters; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindContinuityCheck`
  - optional live `kind` continuity ctest for the topology-driven Kubernetes worlds harness
  - proves resume routing and logical-session continuity over `gateway-1` after migration/transfer flows; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindMultiGatewayCheck`
  - optional live `kind` multi-gateway ctest for the topology-driven Kubernetes worlds harness
  - proves continuity survives a gateway hop by resuming through `gateway-2` after a `gateway-1` login; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindRestartCheck`
  - optional live `kind` restart ctest for the topology-driven Kubernetes worlds harness
  - proves simple fault-injection continuity rehearsal across gateway and server rollouts; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindMetricsBudgetCheck`
  - optional live `kind` metrics-budget ctest for the topology-driven Kubernetes worlds harness
  - proves the current multi-gateway migration/transfer stories also emit the expected gateway/server success counters and avoid unexpected fallback growth; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindLocatorFallbackCheck`
  - optional live `kind` locator-fallback ctest for the topology-driven Kubernetes worlds harness
  - proves continuity survives exact alias-key loss by falling back to the locator hint plus selector hit/fallback path; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindWorldStateFallbackCheck`
  - optional live `kind` world-state fallback ctest for the topology-driven Kubernetes worlds harness
  - proves continuity survives missing persisted world/world-owner state by falling back safely on the original backend and recording explicit reason metrics; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindRedisOutageCheck`
  - optional live `kind` Redis dependency outage ctest for the topology-driven Kubernetes worlds harness
  - proves `admin_app` detects Redis loss, registry-backed endpoints degrade, and observed topology recovers after Redis returns; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindWorkerOutageCheck`
  - optional live `kind` worker metrics outage ctest for the topology-driven Kubernetes worlds harness
  - proves `admin_app` detects `wb_worker` metrics-path loss, the worker endpoint degrades, and the snapshot recovers after the path returns; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindGatewayIngressImpairmentCheck`
  - optional live `kind` gateway ingress impairment ctest for the topology-driven Kubernetes worlds harness
  - proves required Redis loss drops gateway readiness and rejects fresh ingress while one existing bridged session continues to exchange traffic; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindResumeImpairmentCheck`
  - optional live `kind` resume impairment ctest for the topology-driven Kubernetes worlds harness
  - proves required Redis loss blocks cross-gateway continuity resume for a disconnected session, then restores the same logical session with usable post-recovery traffic after recovery; skips with return code `77` when prerequisites are unavailable
- `KubernetesWorldsKindMultiFaultImpairmentCheck`
  - optional live `kind` multi-fault impairment ctest for the topology-driven Kubernetes worlds harness
  - proves required Redis loss plus `gateway-2` pod churn block resume until both recover, then restores the same logical session afterward; skips with return code `77` when prerequisites are unavailable
