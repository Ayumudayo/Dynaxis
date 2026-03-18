# World Drain API

`server/core/worlds/world_drain.hpp`는 live world drain의 phase, 진행 상태, 그리고 transfer/migration surface로의 orchestration handoff를 평가하는 contract를 정의한다.

## Canonical Naming

- canonical include path: `server/core/worlds/world_drain.hpp`
- canonical namespace: `server::core::worlds`
- `server/core/mmorpg/world_drain.hpp`는 compatibility wrapper로만 유지된다

## Scope

- runtime world inventory와 lifecycle policy로부터 drain 진행 상태를 계산한다
- remaining session count와 optional replacement target readiness를 함께 노출한다
- drained completion 뒤에 transfer/migration/clear 중 어떤 surface로 handoff 해야 하는지 계산한다
- owner commit이나 migration payload 해석 자체를 수행하지는 않는다

## Stable Types

- `ObservedWorldDrainInstance`
  - `instance_id`
  - `ready`
  - `active_sessions`
- `ObservedWorldDrainState`
  - `world_id`
  - `owner_instance_id`
  - `draining`
  - `replacement_owner_instance_id`
  - `instances`
- `WorldDrainPhase`
  - `idle`
  - `replacement_target_missing`
  - `replacement_target_not_ready`
  - `draining_sessions`
  - `drained`
- `WorldDrainSummary`
- `WorldDrainStatus`
- `WorldDrainOrchestrationPhase`
- `WorldDrainNextAction`
- `WorldDrainOrchestrationSummary`
- `WorldDrainOrchestrationStatus`
- helper:
  - `evaluate_world_drain(...)`
  - `evaluate_world_drain_orchestration(...)`

## Evaluation Rule

`evaluate_world_drain()`는 아래 우선순위로 phase를 정한다.

1. drain이 선언되지 않았으면 `idle`
2. replacement target이 선언됐지만 observed inventory에 없으면 `replacement_target_missing`
3. replacement target이 선언됐고 present이지만 ready가 아니면 `replacement_target_not_ready`
4. active session이 남아 있으면 `draining_sessions`
5. 그 외에는 `drained`

`evaluate_world_drain_orchestration()`는 drain status에 transfer/migration status를 겹쳐 아래 handoff를 정한다.

1. drain이 없으면 `idle`
2. replacement target이 아직 불안정하면 `blocked_by_replacement_target`
3. 아직 session이 남아 있으면 `draining` / `wait_for_drain`
4. drain은 끝났지만 same-world transfer commit이 남아 있으면 `awaiting_owner_transfer`
5. drain은 끝났지만 migration handoff가 남아 있으면 `awaiting_migration`
6. 그 외에는 `ready_to_clear`

## Current Runtime Mapping

현재 contract는 두 곳에서 사용된다.

- `admin_app`
  - `GET/PUT/DELETE /api/v1/worlds/{world_id}/drain`
  - `GET /api/v1/worlds`
  - `GET /api/v1/topology/observed`
- world lifecycle policy는 여전히 underlying primitive이고, named drain surface는 그 위에 live progress/status와 orchestration handoff를 붙인다
- 현재 지원하는 runtime closure flow는 same-world drain completion 뒤 explicit owner-transfer commit 또는 declared migration readiness를 확인한 다음 `ready_to_clear`로 진입해 named drain policy를 clear하는 형태다

## Non-Goals

- continuity owner commit
- cross-world migration
- gameplay payload handoff
- autoscaling/orchestration

