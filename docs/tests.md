# 테스트 가이드

이 문서는 Dynaxis 저장소의 현재 검증 진입점이다.
세부 시나리오 카탈로그는 각 도구 문서가 기준 문서를 가진다. 특히 loadgen transport proof 형태는 `tools/loadgen/README.md`를 따른다.

`server_core` public package surface를 건드렸다면 이 문서를 `docs/core-api/overview.md`, `docs/core-api/quickstart.md`, `docs/core-api/compatibility-policy.md`, `docs/core-api/checklists.md`와 함께 읽는다. core API 문서는 public package story를 정의하고, 이 문서는 그 story를 저장소 전체 검증 진입점에 연결한다.

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
  - `core_public_api_extensibility_smoke`
  - `core_public_api_realtime_capability_smoke`
  - `core_public_api_worlds_aws_smoke`
- public `server_core` package-first 검증:
  - `ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure`
  - `CoreInstalledPackageConsumer`는 `server_core_installed_consumer`와 `server_core_extensibility_consumer` 둘 다 실행한다
  - `pwsh scripts/run_linux_installed_consumer.ps1`
- additional package extraction 관련 검증:
  - `ctest --test-dir build-windows -C Debug -R "FactoryPgInstalledPackageConsumer|FactoryRedisInstalledPackageConsumer" --output-on-failure`

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
- the only required status check is `windows-fast-tests` from `.github/workflows/ci.yml`.
- path-gated workflows stay non-required on purpose so unrelated PRs do not stall on checks that never dispatch.
- when a PR touches the matching surface, reviewers are expected to wait for the relevant path-gated workflow to pass before merging.

## CI 검증 표면

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
