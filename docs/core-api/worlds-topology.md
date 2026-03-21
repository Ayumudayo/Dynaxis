# 월드 토폴로지(World Topology) API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/worlds/topology.hpp` | `[Stable]` |

## 기준 이름

- 기준 include 경로: `server/core/worlds/topology.hpp`
- 기준 네임스페이스: `server::core::worlds`

## 범위

이 표면은 다음을 하나의 vocabulary로 정의한다.

- desired-topology 문서
- observed-topology 집계
- read-only reconciliation
- read-only actuation plan
- operator-approved actuation request/status
- executor progress/status
- realization/adoption status
- adapter-facing lease/status
- runtime-assignment document/helper

이 계층이 큰 이유는 상태 종류가 많아서가 아니라, 서로 다른 책임을 섞지 않기 위해서다. startup manifest, world lifecycle policy write, concrete orchestrator adapter까지 한 군데에 몰아넣으면 “관측 상태”, “운영자 의도”, “실행 진행 상황”을 구분할 수 없게 된다.

## 공개 계약

### 원하는 토폴로지와 관측 토폴로지

- `DesiredTopologyPool`
  - 하나의 world/shard pool 목표를 표현한다
- `DesiredTopologyDocument`
  - revisioned desired topology 문서다
- `ObservedTopologyInstance`
  - runtime read-model의 최소 입력이다
- `ObservedTopologyPool`
  - instance를 world/shard pool 기준으로 집계한 결과다
- `collect_observed_pools()`
  - observed instance를 runtime pool로 집계한다

### reconciliation과 read-only actuation

- `TopologyPoolStatus`
  - `aligned`
  - `missing_observed_pool`
  - `under_replicated`
  - `over_replicated`
  - `no_ready_instances`
  - `undeclared_observed_pool`
- `reconcile_topology()`
  - desired와 observed를 비교해 reconciliation 결과를 만든다
- `TopologyActuationActionKind`
  - `scale_out_pool`
  - `scale_in_pool`
  - `restore_pool_readiness`
  - `observe_undeclared_pool`
- `plan_topology_actuation()`
  - reconciliation 결과를 read-only next-action plan으로 바꾼다

### operator request와 executor progress

- `TopologyActuationRequestDocument`
  - operator가 승인한 actuation request 문서다
- `TopologyActuationRequestAction`
  - world/shard/action/replica_delta 기준의 요청 한 건이다
- `TopologyActuationRequestActionState`
  - `pending`
  - `satisfied`
  - `superseded`
- `evaluate_topology_actuation_request_status()`
  - 현재 plan과 저장된 request를 비교해 상태를 계산한다

- `TopologyActuationExecutionDocument`
  - executor progress 문서다
- `TopologyActuationExecutionItem`
  - baseline observation과 executor state를 함께 기록한다
- `evaluate_topology_actuation_execution_status()`
  - request 대비 executor 상태를 `available/claimed/completed/failed/stale`로 해석한다

### realization, adapter lease, runtime assignment

- `evaluate_topology_actuation_realization_status()`
  - executor가 완료했다고 보고한 일이 observed topology에 실제 반영됐는지 계산한다
- `TopologyActuationAdapterLeaseDocument`
  - adapter가 현재 claim한 action 집합을 표현한다
- `evaluate_topology_actuation_adapter_status()`
  - execution/realization/lease를 함께 봐서 adapter 상태를 해석한다
- `TopologyActuationRuntimeAssignmentDocument`
  - adapter lease 위에서 concrete instance retarget를 표현한다
- `find_topology_actuation_runtime_assignment()`
  - 특정 `instance_id`에 대응하는 assignment를 찾는다

## 의미 규약

- pool key는 `world_id + shard` 기준이다
- desired 문서가 없으면 observed pool은 모두 `undeclared_observed_pool`로 본다
- `no_ready_instances`는 desired pool이 존재하지만 ready instance가 전혀 없을 때 replica 수 비교보다 우선한다
- actuation plan은 read-only다
  - `missing_observed_pool` / `under_replicated` -> `scale_out_pool`
  - `over_replicated` -> `scale_in_pool`
  - `no_ready_instances` -> `restore_pool_readiness`
  - `undeclared_observed_pool` -> `observe_undeclared_pool`
- request, execution, realization, lease, runtime assignment는 서로 다른 계층이다
  - request: 운영자가 승인한 의도
  - execution: 실행자가 claim/complete/fail 한 진행 상황
  - realization: observed topology 상 실제 반영 여부
  - lease: adapter가 소유한 slice
  - runtime assignment: concrete instance retarget

이 구분이 유지보수에 중요한 이유는, 문제 원인을 단계별로 분리해서 볼 수 있기 때문이다. 예를 들어 “운영자는 scale-out을 승인했지만 executor가 아직 claim하지 않은 문제”와 “executor는 완료했다고 했지만 observed topology에 아직 반영되지 않은 문제”는 완전히 다른 대응을 요구한다.

## 비목표

- startup manifest parsing
- Redis/document storage semantics
- concrete orchestrator coupling
- runtime process spawn/retire side effect
- concrete autoscaler or scheduler adapter
- owner transfer choreography
- migration/session-state handoff

## 공개 검증

- unit contract: `MmorpgTopologyContractTest.*`
- public-api smoke: `core_public_api_smoke`
- installed consumer: `CoreInstalledPackageConsumer`
