# Kubernetes LocalDev Worlds Harness

## Purpose

- reuse the existing topology JSON input for a Kubernetes-first local/dev harness instead of maintaining a second pool layout by hand
- generate repeatable manifests for Redis, Postgres, migrator, write-behind, admin, gateways, and world-server pools
- validate manifest shape deterministically even when no live Kubernetes cluster is available
- provide one narrow `kind`-based local/dev cluster runner above that manifest layer without introducing provider-specific behavior

## Entry Points

- generator:
  - `python scripts/generate_k8s_topology.py --topology-config docker/stack/topologies/default.json`
- PowerShell wrapper:
  - `pwsh scripts/run_k8s_localdev_worlds.ps1`
- live `kind` runner:
  - `pwsh scripts/run_k8s_localdev_kind.ps1 -Action up`
  - `pwsh scripts/run_k8s_localdev_kind.ps1 -Action status`
  - `pwsh scripts/run_k8s_localdev_kind.ps1 -Action down`
- proof:
  - `python tests/python/verify_worlds_kubernetes_localdev.py`
  - optional kubectl client validation:
    - `python tests/python/verify_worlds_kubernetes_localdev.py --kubectl-validate`
    - runs only when `kubectl` has a current context and can reach an API server; otherwise the proof prints a skip message and still treats manifest generation as the supported local/dev check
  - optional live `kind` proof:
    - `python tests/python/verify_worlds_kubernetes_kind.py`
    - skips with code `77` when `docker`, `kubectl`, `kind`, or the local `dynaxis-*:local` images are unavailable
  - optional live `kind` control-plane proof:
    - `python tests/python/verify_worlds_kubernetes_kind_control_plane.py`
    - creates the live cluster, port-forwards `admin-app`, and proves one desired-topology -> actuation -> execution -> adapter -> runtime-assignment roundtrip against observed topology
  - optional live `kind` world-closure proof:
    - `python tests/python/verify_worlds_kubernetes_kind_closure.py`
    - runs a two-stage matrix:
      - default topology: cross-world `drain -> migration -> ready_to_clear`
      - same-world topology: same-world `drain -> transfer commit -> ready_to_clear`
  - optional live `kind` continuity proof:
    - `python tests/python/verify_worlds_kubernetes_kind_continuity.py`
    - runs a two-stage matrix with real gateway resume traffic:
      - default topology: cross-world migration resume preserves continuity through the target world
      - same-world topology: transfer commit resume preserves continuity through the committed replacement owner
  - optional live `kind` multi-gateway continuity proof:
    - `python tests/python/verify_worlds_kubernetes_kind_multigateway.py`
    - runs a two-stage matrix with `gateway-1` login plus `gateway-2` resume:
      - default topology: cross-world migration resume through a different gateway
      - same-world topology: transfer-commit resume through a different gateway
  - optional live `kind` restart proof:
    - `python tests/python/verify_worlds_kubernetes_kind_restart.py`
    - runs a default-topology fault-injection rehearsal:
      - `gateway-1` rollout restart while preserving resume alias through `gateway-2`
      - source world server StatefulSet rollout restart while preserving resume continuity
  - optional live `kind` locator-fallback proof:
    - `python tests/python/verify_worlds_kubernetes_kind_locator_fallback.py`
    - runs a two-stage matrix that deletes the exact resume alias key before reconnect:
      - default topology: source-world locator hint no longer resolves after drain, so `gateway-2` uses selector fallback and still resumes on the migration target
      - same-world topology: locator hint still resolves inside the same world/shard, so `gateway-2` uses selector hit and resumes on the committed replacement owner
  - optional live `kind` world-state fallback proof:
    - `python tests/python/verify_worlds_kubernetes_kind_world_state_fallback.py`
    - runs a two-stage matrix that keeps the exact resume alias binding but deletes persisted continuity state before resume:
      - missing world key: the original backend resumes on its safe default world, resets room residency, and records the `missing_world` fallback reason
      - missing world-owner key: the original backend resumes on its safe default world, resets room residency, rewrites the owner key, and records the `missing_owner` fallback reason
  - optional live `kind` Redis dependency outage proof:
    - `python tests/python/verify_worlds_kubernetes_kind_redis_outage.py`
    - scales Redis down to zero and back up, proving `admin_app` flips `admin_redis_available`, upstream registry-backed endpoints return `503`, `/readyz` stays green because Redis is optional, and observed topology repopulates after Redis returns
  - optional live `kind` worker metrics outage proof:
    - `python tests/python/verify_worlds_kubernetes_kind_worker_outage.py`
    - deletes and reapplies the `wb-worker` metrics service path, proving `admin_app` flips `admin_worker_metrics_available`, `/api/v1/worker/write-behind` returns `503`, `/readyz` stays green because the dependency is optional, and the worker snapshot recovers after the metrics path returns
  - optional live `kind` gateway ingress impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_gateway_ingress_impairment.py`
    - scales Redis down to zero and back up while proving one existing bridged session still exchanges room traffic, but fresh ingress is rejected by `gateway-2` until Redis and gateway readiness recover
  - optional live `kind` resume impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_resume_impairment.py`
    - scales Redis down to zero and back up while proving a disconnected logical session cannot resume through `gateway-2` during the outage, then resumes successfully with the same logical session and usable post-recovery traffic after recovery
  - optional live `kind` multi-fault impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_multifault_impairment.py`
    - combines required Redis loss with `gateway-2` pod churn, proving resume is rejected during the outage, the gateway pod identity changes before Redis returns, and the same logical session resumes once both Redis and the replacement gateway recover
  - optional live `kind` metrics-budget proof:
    - `python tests/python/verify_worlds_kubernetes_kind_metrics_budget.py`
    - runs a two-stage matrix that keeps the current multi-gateway continuity stories but additionally asserts stable metric deltas:
      - default topology: `gateway-1` bind metrics, `gateway-2` resume/world-policy metrics, and target-server migration/state restore success counters
      - same-world topology: `gateway-1` bind metrics, `gateway-2` replacement-selection metrics, and replacement-server owner-restore/state restore success counters
  - preferred evidence runner:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id>`
    - artifact root: `build/k8s-kind-evidence/<run_id>/`
    - baseline capture set records per-proof logs plus `manifest.json` for the current fifteen proof layers
    - focused fallback-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set fallback-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-locator-fallback/locator-fallback.json`
    - focused world-state-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set world-state-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-world-state-fallback/world-state-fallback.json`
    - focused outage-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set outage-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-redis-outage/redis-outage.json`
    - focused worker-outage-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set worker-outage-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-worker-outage/worker-outage.json`
    - focused impairment-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set impairment-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-gateway-ingress-impairment/gateway-ingress-impairment.json`
    - focused resume-impairment-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set resume-impairment-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-resume-impairment/resume-impairment.json`
    - focused multifault-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set multifault-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-multifault-impairment/multifault-impairment.json`
    - focused metrics-only capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set metrics-only`
      - additionally writes `build/k8s-kind-evidence/<run_id>/kind-metrics-budget/metrics-budget.json`

## Outputs

Default output directory: `build/k8s-localdev/`

- `worlds-localdev.generated.yaml`
  - rendered Kubernetes manifest bundle
- `topology.active.json`
  - normalized topology snapshot used for generation
- `summary.json`
  - machine-readable summary of resources, desired pools, and server workloads
- `cluster-state.json`
  - written by the live `kind` runner after apply/readiness succeeds; captures `kubectl get` output for deployments, statefulsets, jobs, services, pods, and configmaps

## Current Scope

- server pools are grouped by `world_id + shard` and rendered as one `StatefulSet` per pool
- gateways, admin-app, write-behind worker, redis, postgres, and migrator are rendered as local/dev support resources
- the harness is intentionally local/dev oriented:
  - Dynaxis runtime images plus local support images (`dynaxis-k8s-postgres:local`, `dynaxis-k8s-redis:local`) use `imagePullPolicy: Never`
  - in-cluster service DNS
  - ephemeral local/dev storage choices
- the live runner:
  - creates or reuses a `kind` cluster
  - loads `dynaxis-server:local`, `dynaxis-gateway:local`, `dynaxis-worker:local`, `dynaxis-admin:local`, `dynaxis-migrator:local`, `dynaxis-k8s-postgres:local`, and `dynaxis-k8s-redis:local`
  - builds the local support images on demand from cached `postgres:16-alpine` / `redis:7-alpine` bases so repeated fresh clusters do not rely on external registry pulls
  - adds dependency `initContainers` so `migrator`, `worker`, `admin`, `gateway`, and `server` wait for Redis/Postgres before cold start
  - applies the generated manifest and waits for postgres, redis, migrator, gateways, admin, worker, and server StatefulSets to become ready

## Current Non-Goals

- no `minikube` or managed-cluster runner yet
- no ingress/load-balancer/provider integration
- no automatic Docker image build step in the `kind` runner; local images must exist already
- no automatic host exposure beyond manual `kubectl port-forward`
- no persistent-volume/storage-class production contract

## Prerequisites

- `docker`, `kubectl`, and `kind` must be installed for the live runner
- the local runtime images must already exist:
  - `dynaxis-server:local`
  - `dynaxis-gateway:local`
  - `dynaxis-worker:local`
  - `dynaxis-admin:local`
  - `dynaxis-migrator:local`
- the runner creates these local support images automatically when needed:
  - `dynaxis-k8s-postgres:local`
  - `dynaxis-k8s-redis:local`
- one simple way to build them is:
  - `pwsh scripts/deploy_docker.ps1 -Action build`

## Validation Story

- deterministic proof:
  - `KubernetesWorldsLocalDevManifestCheck`
- optional live `kind` proof:
  - `KubernetesWorldsKindLiveCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
- optional live `kind` control-plane proof:
  - `KubernetesWorldsKindControlPlaneCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - currently proves topology actuation and runtime assignment only; it does not yet exercise drain/transfer/migration closure on the kind path
- optional live `kind` world-closure proof:
  - `KubernetesWorldsKindClosureCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves named drain closure through migration and transfer orchestration on live kind clusters without pulling in the Docker-only direct client continuity harness
- optional live `kind` continuity proof:
  - `KubernetesWorldsKindContinuityCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves client-observable continuity/resume on live clusters by combining `admin-app` and `gateway-1` port-forward paths with Redis-backed session routing checks
- optional live `kind` multi-gateway continuity proof:
  - `KubernetesWorldsKindMultiGatewayCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves that continuity survives a gateway hop by combining `gateway-1` login, `gateway-2` resume, and Redis-backed routing checks on live clusters
- optional live `kind` restart proof:
  - `KubernetesWorldsKindRestartCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves simple fault-injection continuity rehearsal by combining rollout restarts with Redis-backed alias checks and cross-gateway resume
- optional live `kind` locator-fallback proof:
  - `KubernetesWorldsKindLocatorFallbackCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves continuity survives exact alias-key loss by falling back to the locator hint plus selector hit/fallback path on live clusters
- optional live `kind` world-state fallback proof:
  - `KubernetesWorldsKindWorldStateFallbackCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves continuity survives missing persisted world/world-owner state by falling back safely on the original backend and recording explicit reason metrics
- optional live `kind` Redis dependency outage proof:
  - `KubernetesWorldsKindRedisOutageCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves `admin_app` detects Redis loss, degrades registry-backed endpoints, keeps `/readyz` green, and recovers observed topology after Redis is recreated
- optional live `kind` worker metrics outage proof:
  - `KubernetesWorldsKindWorkerOutageCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves `admin_app` detects `wb_worker` metrics-path loss, degrades the worker endpoint, keeps `/readyz` green, and recovers the worker snapshot after the path is restored
- optional live `kind` gateway ingress impairment proof:
  - `KubernetesWorldsKindGatewayIngressImpairmentCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves required Redis loss makes gateway readiness drop and fresh ingress reject, while one already bridged session can continue until Redis recovers
- optional live `kind` resume impairment proof:
  - `KubernetesWorldsKindResumeImpairmentCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves required Redis loss blocks cross-gateway continuity resume for a disconnected session, then restores the same logical session with usable post-recovery traffic after recovery
- optional live `kind` multi-fault impairment proof:
  - `KubernetesWorldsKindMultiFaultImpairmentCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves required Redis loss plus `gateway-2` pod churn block resume until both recover, then restore the same logical session afterward
- optional live `kind` metrics-budget proof:
  - `KubernetesWorldsKindMetricsBudgetCheck`
  - skips cleanly when live-cluster prerequisites are not available on the machine
  - proves the current migration/transfer continuity stories also emit the expected gateway/server success counters without unexpected fallback growth
- optional client-side schema/render validation when `kubectl` is available:
  - `kubectl create --dry-run=client -f build/k8s-localdev/worlds-localdev.generated.yaml -o yaml`
  - this path is best-effort only because current `kubectl` still needs API discovery for resource mapping; no-context or unreachable-cluster environments skip it instead of failing the local/dev harness proof
- preferred manual/release evidence capture:
  - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id>`
  - writes `build/k8s-kind-evidence/<run_id>/manifest.json` plus one log file per proof
  - the world-state fallback proof also writes `kind-world-state-fallback/world-state-fallback.json` when the evidence runner executes it
  - the metrics-budget proof also writes `kind-metrics-budget/metrics-budget.json` when the evidence runner executes it

## Host Access

- the live `kind` runner currently proves apply/readiness only
- use manual port-forward when host access is needed:
  - `kubectl --context kind-dynaxis-localdev -n dynaxis-localdev port-forward svc/admin-app 39200:39200`
  - `kubectl --context kind-dynaxis-localdev -n dynaxis-localdev port-forward svc/gateway-1 6000:6000`

## Follow-Up

- the next Phase 6 slice should add a broader transport-level or cross-cluster impairment rehearsal beyond the current ingress/resume multi-fault path, optional-dependency outage, alias-loss, world-state-fallback, restart, and metrics-budget path
- the runner intentionally stays separate from the topology-to-manifest generator so the render step remains reusable in CI or future provider adapters
