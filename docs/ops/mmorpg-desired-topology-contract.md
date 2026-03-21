# MMORPG 목표 토폴로지(Desired Topology) 계약

이 문서는 MMORPG 런타임의 목표 토폴로지(desired topology) 계약을 정의한다.
지금은 제어면 문서 API, reconciliation 읽기 모델, 그리고 좁은 범위의 live runtime-assignment 실현 경로가 존재한다.

## 목적

- 현재의 startup 전용 topology manifest가 장기적인 확장 계약이 되어 버리는 것을 막는다.
- 목표 토폴로지(desired topology)를 관측 토폴로지(observed topology), world lifecycle policy와 분리한다.
- 향후 orchestration tranche를 정의하되, Docker 전용이나 Kubernetes 전용 제어면으로 계약을 고정하지 않는다.

## 현재 경계

- startup topology 제어는 여전히 아래 경로를 통해 지원한다.
  - `docker/stack/topologies/*.json`
  - `scripts/deploy_docker.ps1 -TopologyConfig <path>`
- live 목표 토폴로지 문서 제어는 이제 아래 API로 제공된다.
  - `GET /api/v1/topology/desired`
  - `PUT /api/v1/topology/desired`
  - `DELETE /api/v1/topology/desired`
- live 관측 토폴로지 읽기 모델은 아래 API로 제공된다.
  - `GET /api/v1/topology/observed`
- live 목표-대-관측 reconciliation 읽기 모델은 아래 API로 제공된다.
  - `GET /api/v1/topology/reconciliation`
- live 읽기 전용 topology actuation plan은 아래 API로 제공된다.
  - `GET /api/v1/topology/actuation`
- revision이 있는 topology actuation request/status 제어는 아래 API로 제공된다.
  - `GET /api/v1/topology/actuation/request`
  - `PUT /api/v1/topology/actuation/request`
  - `DELETE /api/v1/topology/actuation/request`
  - `GET /api/v1/topology/actuation/status`
- executor 대상 actuation progress/status 제어는 아래 API로 제공된다.
  - `GET /api/v1/topology/actuation/execution`
  - `PUT /api/v1/topology/actuation/execution`
  - `DELETE /api/v1/topology/actuation/execution`
  - `GET /api/v1/topology/actuation/execution/status`
  - `GET /api/v1/topology/actuation/realization`
- adapter 대상 actuation lease/status 제어는 아래 API로 제공된다.
  - `GET /api/v1/topology/actuation/adapter`
  - `PUT /api/v1/topology/actuation/adapter`
  - `DELETE /api/v1/topology/actuation/adapter`
  - `GET /api/v1/topology/actuation/adapter/status`
- Kubernetes 우선 workload orchestration 계약은 아래 헤더로 제공된다.
  - `server/core/worlds/kubernetes.hpp`
- runtime-assignment actuation 제어는 아래 API로 제공된다.
  - `GET /api/v1/topology/actuation/runtime-assignment`
  - `PUT /api/v1/topology/actuation/runtime-assignment`
  - `DELETE /api/v1/topology/actuation/runtime-assignment`
- 현재 manifest는 구체적인 local/proof 산출물이다.
- world lifecycle policy는 별도의 operator/runtime 라우팅 계약으로 남는다.
- 현재 목표 토폴로지는 admin control plane 안의 revisioned 단일 문서로 저장된다. reconciliation 상태는 보이지만, 아직 runtime reconciler나 live pool actuation이 이 문서만 보고 인스턴스를 직접 바꾸지는 않는다.
- 다만 새로운 actuation plan은 필요한 다음 동작을 명시적으로 보여 줘 live-topology blocker를 좁힌다. request/status 표면은 operator가 승인한 동작을 따로 기록하고, execution progress/status 표면은 executor의 claim/ack 진척을 따로 기록하며, realization 표면은 그 claim을 실제 observed topology와 비교한다. adapter 표면은 adapter 대상 lease를 따로 기록하고, runtime-assignment 표면은 유휴 실행 중 서버를 lease된 scale-out pool로 다시 붙일 수 있다. 그래도 여전히 프로세스 spawn/retire나 elastic autoscaling까지 구현하는 것은 아니다.

## 기준 목표 모델

목표 토폴로지는 구체 인스턴스 기반이 아니라 pool 기반이다.

목표 상태가 선언하는 것:

- 어떤 `world_id` / `shard` pool이 있어야 하는가
- 각 pool이 몇 replica를 가져야 하는가
- 선택적 placement/capacity 힌트

목표 상태가 선언하지 않는 것:

- 구체 `instance_id`
- 구체 container/pod 이름
- 구체 host port

## 최소 형태

최상위 필드:

- `topology_id`
- `revision`
- `pools[]`

각 pool 필드:

- `world_id`
- `shard`
- `replicas`
- `capacity_class` 선택
- `placement_tags[]` 선택

## 목표 토폴로지와 관측 토폴로지

- 목표 토폴로지는 "어떤 pool 모양을 원하는가?"에 답한다.
- 관측 토폴로지는 "실제로 어떤 인스턴스가 존재하고 어떤 owner/read-model 상태가 살아 있는가?"에 답한다.
- 구체 인스턴스 배치, health, readiness, owner 가시성은 관측 토폴로지에서만 드러난다.

## World lifecycle policy와의 관계

- 목표 토폴로지가 아래 항목까지 흡수하면 안 된다.
  - `draining`
  - `replacement_owner_instance_id`
  - owner continuity key
- 앞으로의 구현이 목표 토폴로지와 lifecycle policy를 함께 조율할 수는 있어도, 계약은 계속 분리되어야 한다.

## 향후 orchestration 경계

- 앞으로의 tranche는 아래를 추가할 수 있다.
  - pool 지향 scaling API
  - runtime reconciliation / actuation
- 안정된 K8s 계약은 이미 workload binding과 lifecycle phase를 이름 붙여 두었지만, 구체 manifest/controller loop는 아직 후속 작업이다.
- 그 tranche도 계약 수준에서는 orchestrator-agnostic해야 한다.
- 아래 분리는 계속 유지해야 한다.
  - local/proof stack을 위한 startup manifest
  - capacity/placement 의도를 나타내는 목표 토폴로지
  - operator-visible drain/replacement 제어를 위한 lifecycle policy

## 명시적 비목표

- 목표 토폴로지에 구체 `instance_id`를 넣지 않는다.
- host port 선언을 넣지 않는다.
- 계약 자체에 Docker 전용이나 Kubernetes 전용 의미를 넣지 않는다.
- 목표 토폴로지가 반드시 Redis에 저장되어야 한다고 요구하지 않는다.
- 이 문서에서 live scaling API 형태를 확정하지 않는다.

## 현재 Admin API 형태

`desired topology` 문서:

- `topology_id`
- `revision`
- `updated_at_ms`
- `pools[]`

각 pool 필드:

- `world_id`
- `shard`
- `replicas`
- `capacity_class` 선택
- `placement_tags[]` 선택

쓰기 semantics:

- `PUT /api/v1/topology/desired`는 `topology_id`와 `pools[]`를 가진 JSON body를 요구한다.
- 선택적 `expected_revision`으로 낙관적 revision 검사를 한다.
- 성공한 쓰기는 `revision = current_revision + 1`을 할당한다.
- `DELETE /api/v1/topology/desired`는 저장된 문서를 지운다.

관측 토폴로지 읽기 모델:

- `GET /api/v1/topology/observed`
- 현재 `instances[]`, `worlds[]`, `summary`, `updated_at_ms`를 반환한다.
- 의도적으로 desired 문서와 world lifecycle policy 쓰기와 분리되어 있다.

reconciliation 읽기 모델:

- `GET /api/v1/topology/reconciliation`
- 저장된 desired 문서, 집계된 observed pool, desired-vs-observed 정렬 요약을 반환한다.
- 아직은 읽기 전용 증거 표면일 뿐이며, runtime mutation이나 pool scaling을 직접 일으키지 않는다.

actuation 읽기 모델:

- `GET /api/v1/topology/actuation`
- 저장된 desired 문서, observed pool, `scale_out_pool`, `scale_in_pool`, `restore_pool_readiness`, `observe_undeclared_pool` 같은 다음 action 목록을 읽기 전용으로 반환한다.
- mismatch를 명시적인 action vocabulary로 바꿔 blocker를 줄여 주지만, 여전히 runtime topology를 직접 바꾸지는 않는다.

actuation request 문서:

- `GET /api/v1/topology/actuation/request`
- `PUT /api/v1/topology/actuation/request`
- `DELETE /api/v1/topology/actuation/request`
- 아래 필드를 가진 별도 revisioned operator 승인 문서를 저장한다.
  - `request_id`
  - `revision`
  - `requested_at_ms`
  - `basis_topology_revision`
  - `actions[]` with `world_id`, `shard`, `action`, `replica_delta`
- `PUT`은 `GET /api/v1/topology/actuation`에서 현재 actionable한 항목만 받는다.
- `observe_undeclared_pool`은 관찰 전용 범주이므로 쓰기 대상이 아니다.

actuation status 읽기 모델:

- `GET /api/v1/topology/actuation/status`
- 아래를 반환한다.
  - 현재 desired topology
  - 현재 저장된 actuation request 문서
  - observed pool
  - 현재 읽기 전용 actuation plan 요약
  - 각 request action의 상태: `pending`, `satisfied`, `superseded`
- 여전히 scaling을 실행하지 않으며, 앞으로 runtime reconciler나 외부 orchestrator adapter가 읽을 수 있는 최소 request/status 계약만 정의한다.

execution progress 문서:

- `GET /api/v1/topology/actuation/execution`
- `PUT /api/v1/topology/actuation/execution`
- `DELETE /api/v1/topology/actuation/execution`
- 아래 필드를 가진 별도 revisioned executor 진행 문서를 저장한다.
  - `executor_id`
  - `revision`
  - `updated_at_ms`
  - `request_revision`
  - `actions[]` with `world_id`, `shard`, `action`, `replica_delta`, `state`
- `PUT`은 현재 저장된 actuation request revision과 일치하는 항목만 받는다.
- 이 표면은 여전히 orchestrator-agnostic하며, container/pod/cloud API가 아니라 진행 상태만 이름 붙인다.
- `observed_instances_before` / `ready_instances_before`는 executor가 claim 시점에 본 기준 상태를 기록해, 나중 realization이 현재 observed topology와 비교할 수 있게 한다.

execution status 읽기 모델:

- `GET /api/v1/topology/actuation/execution/status`
- 아래를 반환한다.
  - 현재 저장된 actuation request 문서
  - 현재 저장된 execution progress 문서
  - 현재 request-summary
  - 각 request의 execution 상태: `available`, `claimed`, `completed`, `failed`, `stale`
- 이 표면은 executor 대상 claim/ack/status 계약만 정의하며, 아직 live scaling을 수행하지는 않는다.

realization 읽기 모델:

- `GET /api/v1/topology/actuation/realization`
- 아래를 반환한다.
  - 현재 저장된 actuation request 문서
  - 현재 저장된 execution progress 문서
  - 현재 observed pool
  - 현재 request-summary
  - 현재 execution-summary
  - 각 request의 realization 상태: `available`, `claimed`, `awaiting_observation`, `realized`, `failed`, `stale`
- 이 표면은 executor가 스스로 완료했다고 말한 것과 실제 observed-topology 채택이 일어났는지를 처음으로 분리해 보여 준다.

adapter lease 문서:

- `GET /api/v1/topology/actuation/adapter`
- `PUT /api/v1/topology/actuation/adapter`
- `DELETE /api/v1/topology/actuation/adapter`
- 아래 필드를 가진 별도 revisioned adapter lease 문서를 저장한다.
  - `adapter_id`
  - `revision`
  - `leased_at_ms`
  - `execution_revision`
  - `actions[]` with `world_id`, `shard`, `action`, `replica_delta`
- `PUT`은 현재 execution revision과 현재 non-stale execution/realization 표면에 맞는 항목만 받는다.
- 이 표면도 orchestrator-agnostic하며, 구체 cloud/container API payload가 아니라 lease/claim 의도만 기록한다.

adapter status 읽기 모델:

- `GET /api/v1/topology/actuation/adapter/status`
- 아래를 반환한다.
  - 현재 저장된 actuation adapter lease 문서
  - 현재 저장된 execution progress 문서
  - 현재 execution-summary
  - 현재 realization-summary
  - 각 action의 adapter 상태: `available`, `leased`, `awaiting_realization`, `realized`, `failed`, `stale`
- 이 표면은 execution + realization 증거 위에 얹히는 가장 좁은 adapter 대상 lease/status 경계를 정의한다. 아직 runtime scale change 자체를 수행하지는 않는다.

runtime assignment 제어:

- `GET /api/v1/topology/actuation/runtime-assignment`
- `PUT /api/v1/topology/actuation/runtime-assignment`
- `DELETE /api/v1/topology/actuation/runtime-assignment`
- 아래 필드를 가진 revisioned runtime-assignment 문서를 저장한다.
  - `adapter_id`
  - `revision`
  - `updated_at_ms`
  - `lease_revision`
  - `assignments[]` with `instance_id`, `world_id`, `shard`, `action`
- 현재 구현은 의도적으로 좁다.
  - 새 프로세스를 spawn하거나 기존 프로세스를 retire하지 않는다.
  - 이미 떠 있는 유휴 서버를 lease된 scale-out pool로 다시 붙이는 경로만 제공한다.
