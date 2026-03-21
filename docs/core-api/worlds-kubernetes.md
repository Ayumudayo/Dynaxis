# 월드 Kubernetes API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/worlds/kubernetes.hpp` | `[Stable]` |

## 기준 이름

- 기준 include 경로: `server/core/worlds/kubernetes.hpp`
- 기준 네임스페이스: `server::core::worlds`

## 범위

- desired-topology / actuation / runtime-assignment / drain-orchestration 계층을 Kubernetes workload lifecycle vocabulary로 해석한다
- workload binding, runtime observation counter, replica patch / readiness wait / runtime assignment / drain / owner-transfer / migration / retirement phase evaluation을 다룬다
- 의도적으로 read-only, contract-first 계층으로 유지한다

포함하지 않는 것:

- `kubectl` 호출
- watch loop
- CRD / Helm chart
- cloud-provider API
- manifest persistence

즉 이 계층은 “Kubernetes에서 무엇을 해야 하는가”를 직접 실행하는 코드가 아니라, “현재 상태를 Kubernetes 관점에서 어떻게 읽어야 하는가”를 정규화하는 계약이다.

## 공개 계약

- `KubernetesWorkloadKind`
  - `deployment`
  - `statefulset`
- `KubernetesPoolBinding`
  - world pool 하나를 namespace/workload에 연결한 binding
- `make_kubernetes_pool_binding()`
  - `DesiredTopologyPool` 하나에서 canonical binding을 만든다
- `make_kubernetes_pool_workload_name()`
  - `world_id + shard` 기반 sanitized workload 이름을 만든다
- `KubernetesPoolObservation`
  - `current_spec_replicas`
  - `ready_replicas`
  - `available_replicas`
  - `terminating_replicas`
  - `assigned_runtime_instances`
  - `idle_ready_runtime_instances`
- `count_topology_actuation_runtime_assignments()`
  - 특정 `world_id + shard + action`에 대한 runtime assignment 수를 센다
- `KubernetesPoolOrchestrationPhase`
  - `idle`
  - `scale_workload`
  - `await_ready_replicas`
  - `await_runtime_assignment`
  - `await_replacement_target`
  - `await_drain`
  - `await_owner_transfer`
  - `await_migration`
  - `retire_workload`
  - `complete`
  - `stale`
- `KubernetesPoolNextAction`
  - `none`
  - `patch_workload_replicas`
  - `wait_for_pods_ready`
  - `publish_runtime_assignments`
  - `stabilize_replacement_target`
  - `wait_for_drain`
  - `commit_owner_transfer`
  - `wait_for_migration`
  - `patch_workload_retirement`
- `evaluate_kubernetes_pool_orchestration()`
  - binding, 현재 workload/runtime 관측값, optional adapter lease action, optional `WorldDrainOrchestrationStatus`를 합쳐 Kubernetes-first orchestration phase와 next action을 계산한다

## 의미 규약

- `scale_out_pool`
  - target replica보다 spec replica가 적으면 `scale_workload`
  - spec replica는 맞지만 pod readiness가 부족하면 `await_ready_replicas`
  - pod는 준비됐지만 runtime assignment가 부족하면 `await_runtime_assignment`
  - 모두 만족하면 `complete`
- `restore_pool_readiness`
  - replica 수는 맞는데 준비된 pod가 없거나 부족한 경우 readiness 회복 경로로 본다
- `scale_in_pool`
  - world drain / transfer / migration 상태를 함께 봐서 바로 retire 가능한지, 아직 replacement target/drain/owner transfer/migration을 기다려야 하는지 판단한다
- `observe_undeclared_pool`
  - read-only 관찰 상태로 남긴다

이렇게 phase를 세분화하는 이유는 “Kubernetes 쪽에서 해야 할 일”과 “runtime/world lifecycle 쪽에서 아직 안 끝난 일”을 구분하기 위해서다. 그렇지 않으면 scale-in 같은 단순 표현 뒤에 여러 종류의 미완료 상태가 숨어 버린다.

## 비목표

- live cluster mutation loop 자체
- concrete `kubectl` / API server protocol
- cloud-specific load balancer / database orchestration
- manifest 저장소 구현

## 공개 검증

- public-api smoke: `core_public_api_smoke`
- dedicated contract/unit proof: `tests/core/test_worlds_kubernetes.cpp`
- installed consumer proof: `CoreInstalledPackageConsumer`
