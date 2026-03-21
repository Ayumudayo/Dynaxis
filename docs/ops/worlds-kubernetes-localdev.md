# Kubernetes 로컬 개발 Worlds 하네스

## 목적

- 기존 topology JSON 입력을 그대로 재사용해, 손으로 별도 pool 배치를 유지하지 않고 Kubernetes 우선 local/dev 하네스를 만든다.
- Redis, Postgres, migrator, write-behind, admin, gateway, world-server pool에 대한 반복 가능한 manifest를 생성한다.
- 실제 Kubernetes 클러스터가 없어도 manifest 형태를 결정론적으로 검증한다.
- manifest 계층 위에 provider 특화 동작을 넣지 않는 좁은 `kind` 기반 local/dev 클러스터 실행기를 제공한다.

## 진입점

- 생성기:
  - `python scripts/generate_k8s_topology.py --topology-config docker/stack/topologies/default.json`
- PowerShell 래퍼:
  - `pwsh scripts/run_k8s_localdev_worlds.ps1`
- live `kind` 실행기:
  - `pwsh scripts/run_k8s_localdev_kind.ps1 -Action up`
  - `pwsh scripts/run_k8s_localdev_kind.ps1 -Action status`
  - `pwsh scripts/run_k8s_localdev_kind.ps1 -Action down`
- proof:
  - `python tests/python/verify_worlds_kubernetes_localdev.py`
  - 선택적 `kubectl` 클라이언트 검증:
    - `python tests/python/verify_worlds_kubernetes_localdev.py --kubectl-validate`
    - `kubectl`에 현재 context가 있고 API server에 닿을 수 있을 때만 실행한다. 그렇지 않으면 skip 메시지를 출력하고, manifest 생성 자체를 지원되는 local/dev 검증으로 본다.
  - 선택적 live `kind` proof:
    - `python tests/python/verify_worlds_kubernetes_kind.py`
    - `docker`, `kubectl`, `kind`, 또는 로컬 `dynaxis-*:local` 이미지가 없으면 코드 `77`로 skip한다.
  - 선택적 live `kind` control-plane proof:
    - `python tests/python/verify_worlds_kubernetes_kind_control_plane.py`
    - live cluster를 만들고 `admin-app`을 port-forward한 뒤, desired-topology -> actuation -> execution -> adapter -> runtime-assignment가 observed topology까지 roundtrip되는지 증명한다.
  - 선택적 live `kind` world-closure proof:
    - `python tests/python/verify_worlds_kubernetes_kind_closure.py`
    - 두 단계 행렬을 돌린다.
      - 기본 topology: cross-world `drain -> migration -> ready_to_clear`
      - same-world topology: same-world `drain -> transfer commit -> ready_to_clear`
  - 선택적 live `kind` continuity proof:
    - `python tests/python/verify_worlds_kubernetes_kind_continuity.py`
    - 실제 gateway resume 트래픽을 사용한 두 단계 행렬을 돌린다.
      - 기본 topology: cross-world migration resume이 target world를 통해 continuity를 보존
      - same-world topology: transfer commit resume이 committed replacement owner를 통해 continuity를 보존
  - 선택적 live `kind` multi-gateway continuity proof:
    - `python tests/python/verify_worlds_kubernetes_kind_multigateway.py`
    - `gateway-1` 로그인 + `gateway-2` resume을 사용하는 두 단계 행렬을 돌린다.
      - 기본 topology: 다른 gateway를 통한 cross-world migration resume
      - same-world topology: 다른 gateway를 통한 transfer-commit resume
  - 선택적 live `kind` restart proof:
    - `python tests/python/verify_worlds_kubernetes_kind_restart.py`
    - 기본 topology fault-injection rehearsal을 돌린다.
      - `gateway-1` rollout restart 중에도 `gateway-2`를 통한 resume alias 유지
      - source world server StatefulSet rollout restart 중에도 resume continuity 유지
  - 선택적 live `kind` locator-fallback proof:
    - `python tests/python/verify_worlds_kubernetes_kind_locator_fallback.py`
    - 정확한 resume alias key를 reconnect 전에 지우는 두 단계 행렬을 돌린다.
      - 기본 topology: source-world locator hint가 drain 후 더 이상 직접 해석되지 않아도 `gateway-2`가 selector fallback으로 migration target에서 resume
      - same-world topology: locator hint가 같은 world/shard 안에서는 여전히 유효해 `gateway-2`가 selector hit로 committed replacement owner에서 resume
  - 선택적 live `kind` world-state fallback proof:
    - `python tests/python/verify_worlds_kubernetes_kind_world_state_fallback.py`
    - 정확한 resume alias binding은 유지하되 persisted continuity state를 resume 전에 지우는 두 단계 행렬을 돌린다.
      - missing world key: 원래 backend가 safe default world로 resume하고 room residency를 재설정하며 `missing_world` fallback reason을 기록
      - missing world-owner key: 원래 backend가 safe default world로 resume하고 room residency를 재설정하고 owner key를 다시 쓰며 `missing_owner` fallback reason을 기록
  - 선택적 live `kind` Redis dependency outage proof:
    - `python tests/python/verify_worlds_kubernetes_kind_redis_outage.py`
    - Redis를 0개로 줄였다가 다시 올리며, `admin_app`이 `admin_redis_available`을 뒤집고, registry 기반 엔드포인트가 `503`을 돌리며, `/readyz`는 Redis가 optional이므로 green을 유지하고, Redis 복구 후 observed topology가 다시 채워지는지 증명한다.
  - 선택적 live `kind` worker metrics outage proof:
    - `python tests/python/verify_worlds_kubernetes_kind_worker_outage.py`
    - `wb-worker` metrics service 경로를 삭제했다가 다시 적용해, `admin_app`이 `admin_worker_metrics_available`을 뒤집고 `/api/v1/worker/write-behind`가 `503`을 돌리며 `/readyz`는 green을 유지하고 경로 복구 후 worker snapshot이 돌아오는지 증명한다.
  - 선택적 live `kind` gateway ingress impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_gateway_ingress_impairment.py`
    - Redis를 0개로 줄였다가 다시 올리는 동안, 이미 브리지된 세션 하나는 계속 room 트래픽을 주고받지만 새 ingress는 `gateway-2`에서 거절되고 Redis/gateway readiness가 회복된 뒤에만 다시 받아들여지는지 증명한다.
  - 선택적 live `kind` resume impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_resume_impairment.py`
    - Redis를 0개로 줄였다가 다시 올리는 동안, 끊긴 논리 세션은 장애 중 `gateway-2`로 resume하지 못하고, 복구 후에는 같은 논리 세션으로 usable traffic과 함께 resume하는지 증명한다.
  - 선택적 live `kind` multi-fault impairment proof:
    - `python tests/python/verify_worlds_kubernetes_kind_multifault_impairment.py`
    - 필수 Redis 손실과 `gateway-2` pod churn을 함께 만들어, 두 장애가 모두 복구되기 전까지는 resume이 거절되고 둘 다 회복된 뒤 같은 논리 세션이 다시 살아나는지 증명한다.
  - 선택적 live `kind` metrics-budget proof:
    - `python tests/python/verify_worlds_kubernetes_kind_metrics_budget.py`
    - 현재 multi-gateway continuity 이야기를 유지한 채 metric delta까지 안정적으로 맞는지 보는 두 단계 행렬을 돌린다.
      - 기본 topology: `gateway-1` bind metrics, `gateway-2` resume/world-policy metrics, target-server migration/state restore success counter
      - same-world topology: `gateway-1` bind metrics, `gateway-2` replacement-selection metrics, replacement-server owner-restore/state restore success counter
  - 권장 증거 실행기:
    - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id>`
    - 산출물 루트: `build/k8s-kind-evidence/<run_id>/`
    - 기본 capture set은 현재 proof 15개 계층에 대한 로그와 `manifest.json`을 함께 남긴다.
    - 집중 fallback 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set fallback-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-locator-fallback/locator-fallback.json` 생성
    - 집중 world-state 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set world-state-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-world-state-fallback/world-state-fallback.json` 생성
    - 집중 outage 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set outage-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-redis-outage/redis-outage.json` 생성
    - 집중 worker-outage 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set worker-outage-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-worker-outage/worker-outage.json` 생성
    - 집중 impairment 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set impairment-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-gateway-ingress-impairment/gateway-ingress-impairment.json` 생성
    - 집중 resume-impairment 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set resume-impairment-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-resume-impairment/resume-impairment.json` 생성
    - 집중 multifault 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set multifault-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-multifault-impairment/multifault-impairment.json` 생성
    - 집중 metrics 전용 capture:
      - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id> --capture-set metrics-only`
      - 추가로 `build/k8s-kind-evidence/<run_id>/kind-metrics-budget/metrics-budget.json` 생성

## 출력물

기본 출력 디렉터리: `build/k8s-localdev/`

- `worlds-localdev.generated.yaml`
  - 렌더링된 Kubernetes manifest 묶음
- `topology.active.json`
  - 생성에 사용된 정규화 topology 스냅샷
- `summary.json`
  - 리소스, desired pool, server workload의 기계 판독 요약
- `cluster-state.json`
  - live `kind` 실행기가 apply/readiness 성공 후 쓰는 파일이며, deployment, statefulset, job, service, pod, configmap에 대한 `kubectl get` 출력을 담는다.

## 현재 범위

- server pool은 `world_id + shard` 기준으로 묶이고, pool당 하나의 `StatefulSet`으로 렌더링된다.
- gateway, admin-app, write-behind worker, redis, postgres, migrator도 local/dev 지원 리소스로 함께 렌더링된다.
- 이 하네스는 의도적으로 local/dev 지향이다.
  - Dynaxis runtime 이미지와 local support 이미지(`dynaxis-k8s-postgres:local`, `dynaxis-k8s-redis:local`)는 `imagePullPolicy: Never`를 쓴다.
  - in-cluster service DNS를 전제로 한다.
  - 저장소는 local/dev 성격의 임시 선택을 사용한다.
- live 실행기는 아래를 수행한다.
  - `kind` cluster를 만들거나 재사용
  - `dynaxis-server:local`, `dynaxis-gateway:local`, `dynaxis-worker:local`, `dynaxis-admin:local`, `dynaxis-migrator:local`, `dynaxis-k8s-postgres:local`, `dynaxis-k8s-redis:local`을 로드
  - 반복 실행 시 외부 registry pull에 의존하지 않도록, 캐시된 `postgres:16-alpine` / `redis:7-alpine`를 기반으로 local support 이미지를 필요 시 빌드
  - `migrator`, `worker`, `admin`, `gateway`, `server`가 cold start에서 Redis/Postgres를 먼저 기다리도록 dependency `initContainers` 추가
  - 생성된 manifest를 적용하고 postgres, redis, migrator, gateway, admin, worker, server StatefulSet이 ready가 될 때까지 대기

## 현재 비목표

- 아직 `minikube` 또는 managed-cluster 실행기는 없다.
- ingress/load-balancer/provider 통합은 없다.
- `kind` 실행기 안에 자동 Docker 이미지 빌드 단계는 없다. 필요한 local 이미지는 이미 존재해야 한다.
- 수동 `kubectl port-forward` 외의 자동 host 노출은 없다.
- persistent volume / storage class의 production 계약은 아직 없다.

## 사전 요구 조건

- live 실행기에는 `docker`, `kubectl`, `kind`가 필요하다.
- 아래 local runtime 이미지가 이미 있어야 한다.
  - `dynaxis-server:local`
  - `dynaxis-gateway:local`
  - `dynaxis-worker:local`
  - `dynaxis-admin:local`
  - `dynaxis-migrator:local`
- 아래 local support 이미지는 필요 시 실행기가 자동 생성한다.
  - `dynaxis-k8s-postgres:local`
  - `dynaxis-k8s-redis:local`
- 가장 단순한 빌드 방법:
  - `pwsh scripts/deploy_docker.ps1 -Action build`

## 검증 이야기

- 결정론적 proof:
  - `KubernetesWorldsLocalDevManifestCheck`
- 선택적 live `kind` proof:
  - `KubernetesWorldsKindLiveCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
- 선택적 live `kind` control-plane proof:
  - `KubernetesWorldsKindControlPlaneCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - 현재는 topology actuation과 runtime assignment만 증명하며, kind 경로에서 drain/transfer/migration closure 전체를 함께 돌리지는 않는다.
- 선택적 live `kind` world-closure proof:
  - `KubernetesWorldsKindClosureCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - Docker 전용 direct client continuity 하네스를 끌어오지 않고도, live kind cluster에서 named drain closure를 migration/transfer orchestration으로 증명한다.
- 선택적 live `kind` continuity proof:
  - `KubernetesWorldsKindContinuityCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - `admin-app`과 `gateway-1` port-forward 경로, Redis 기반 session routing 검사를 합쳐 live cluster에서 continuity/resume을 client가 실제로 체감하는 수준으로 증명한다.
- 선택적 live `kind` multi-gateway continuity proof:
  - `KubernetesWorldsKindMultiGatewayCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - `gateway-1` 로그인, `gateway-2` resume, Redis 기반 routing 검사를 합쳐 gateway hop 뒤에도 continuity가 유지되는지 증명한다.
- 선택적 live `kind` restart proof:
  - `KubernetesWorldsKindRestartCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - rollout restart와 Redis 기반 alias 검사, cross-gateway resume을 합쳐 fault-injection continuity rehearsal을 증명한다.
- 선택적 live `kind` locator-fallback proof:
  - `KubernetesWorldsKindLocatorFallbackCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - 정확한 alias key를 잃었을 때도 locator hint + selector hit/fallback 경로로 continuity가 유지되는지 증명한다.
- 선택적 live `kind` world-state fallback proof:
  - `KubernetesWorldsKindWorldStateFallbackCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - persisted world/world-owner 상태가 없을 때 continuity가 안전하게 fallback되고 명시적 reason metric이 남는지 증명한다.
- 선택적 live `kind` Redis dependency outage proof:
  - `KubernetesWorldsKindRedisOutageCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - `admin_app`이 Redis 손실을 감지하고, registry 기반 endpoint를 저하시켜도 `/readyz`는 green을 유지하며, Redis 복구 후 observed topology도 돌아오는지 증명한다.
- 선택적 live `kind` worker metrics outage proof:
  - `KubernetesWorldsKindWorkerOutageCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - `admin_app`이 `wb_worker` metrics path 손실을 감지하고 worker endpoint를 저하시켜도 `/readyz`는 green을 유지하며, metrics path 복구 후 worker snapshot도 돌아오는지 증명한다.
- 선택적 live `kind` gateway ingress impairment proof:
  - `KubernetesWorldsKindGatewayIngressImpairmentCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - Redis 손실 동안 gateway readiness가 내려가고 fresh ingress가 거절되지만, 이미 브리지된 세션 하나는 계속 살 수 있는지 증명한다.
- 선택적 live `kind` resume impairment proof:
  - `KubernetesWorldsKindResumeImpairmentCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - Redis 손실 동안 cross-gateway continuity resume이 막히고, 복구 후에는 같은 논리 세션이 usable traffic과 함께 살아나는지 증명한다.
- 선택적 live `kind` multi-fault impairment proof:
  - `KubernetesWorldsKindMultiFaultImpairmentCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - Redis 손실과 `gateway-2` pod churn이 동시에 있을 때, 둘 다 복구되기 전까지는 resume이 거절되고 둘 다 복구된 뒤 같은 논리 세션이 다시 살아나는지 증명한다.
- 선택적 live `kind` metrics-budget proof:
  - `KubernetesWorldsKindMetricsBudgetCheck`
  - live-cluster 사전 조건이 없으면 깨끗하게 skip한다.
  - 현재 migration/transfer continuity 이야기에서 기대한 gateway/server success counter가 unexpected fallback 증가 없이 남는지 증명한다.
- 선택적 client-side schema/render 검증:
  - `kubectl create --dry-run=client -f build/k8s-localdev/worlds-localdev.generated.yaml -o yaml`
  - 현재 `kubectl`은 resource mapping을 위해 API discovery가 여전히 필요하므로, context가 없거나 unreachable cluster 환경에서는 실패 대신 skip한다.
- 권장 수동/release 증거 캡처:
  - `python tests/python/capture_worlds_kubernetes_kind_evidence.py --run-id <run_id>`
  - `build/k8s-kind-evidence/<run_id>/manifest.json`과 proof별 로그를 기록한다.
  - world-state fallback proof를 실행하면 `kind-world-state-fallback/world-state-fallback.json`도 같이 쓴다.
  - metrics-budget proof를 실행하면 `kind-metrics-budget/metrics-budget.json`도 같이 쓴다.

## 호스트 접근

- live `kind` 실행기는 현재 apply/readiness까지만 증명한다.
- 호스트 접근이 필요하면 수동 port-forward를 사용한다.
  - `kubectl --context kind-dynaxis-localdev -n dynaxis-localdev port-forward svc/admin-app 39200:39200`
  - `kubectl --context kind-dynaxis-localdev -n dynaxis-localdev port-forward svc/gateway-1 6000:6000`

## 후속 과제

- 다음 Phase 6 구간에서는 현재 ingress/resume multifault 경로, optional-dependency outage, alias-loss, world-state-fallback, restart, metrics-budget 경로를 넘어서는 더 넓은 transport 수준 또는 cross-cluster impairment rehearsal을 추가해야 한다.
- 실행기는 의도적으로 topology-to-manifest 생성기와 분리되어 있다. 그래야 render 단계 자체를 CI나 future provider adapter에서도 재사용할 수 있다.
