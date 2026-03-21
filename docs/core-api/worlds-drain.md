# 월드 드레인(World Drain) API

`server/core/worlds/world_drain.hpp`는 live world drain의 phase, 진행 상태, 그리고 transfer/migration surface로의 orchestration handoff를 평가하는 계약이다.

## 기준 이름

- 기준 include 경로: `server/core/worlds/world_drain.hpp`
- 기준 네임스페이스: `server::core::worlds`

## 범위

- runtime world inventory와 lifecycle policy를 보고 drain 진행 상태를 계산한다
- remaining session 수와 optional replacement target readiness를 함께 본다
- drain 완료 뒤 transfer / migration / clear 중 어떤 surface로 handoff 해야 하는지 계산한다
- owner commit이나 migration payload 해석 자체는 여기서 하지 않는다

이렇게 분리하는 이유는 drain 진행 판단과 실제 handoff 실행을 섞지 않기 위해서다. 둘을 한 계층에 넣으면 상태 판정 규칙과 실행 부작용이 강하게 엉켜 테스트와 운영 판단이 어려워진다.

## 안정 타입

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
- helper
  - `evaluate_world_drain(...)`
  - `evaluate_world_drain_orchestration(...)`

## 평가 규칙

`evaluate_world_drain()`는 아래 우선순위로 phase를 정한다.

1. drain이 선언되지 않았으면 `idle`
2. replacement target이 선언됐지만 observed inventory에 없으면 `replacement_target_missing`
3. replacement target이 present이지만 ready가 아니면 `replacement_target_not_ready`
4. active session이 남아 있으면 `draining_sessions`
5. 그 외에는 `drained`

`evaluate_world_drain_orchestration()`는 drain status 위에 transfer/migration status를 겹쳐 다음 handoff를 정한다.

1. drain이 없으면 `idle`
2. replacement target이 아직 불안정하면 `blocked_by_replacement_target`
3. 아직 session이 남아 있으면 `draining`
4. drain은 끝났지만 same-world transfer commit이 남아 있으면 `awaiting_owner_transfer`
5. drain은 끝났지만 migration handoff가 남아 있으면 `awaiting_migration`
6. 그 외에는 `ready_to_clear`

## 현재 런타임 매핑

현재 이 계약은 주로 다음 경로에서 사용된다.

- `admin_app`
  - `GET/PUT/DELETE /api/v1/worlds/{world_id}/drain`
  - `GET /api/v1/worlds`
  - `GET /api/v1/topology/observed`

현재 지원하는 closure flow는 “drain 진행 판단 -> transfer 또는 migration readiness 확인 -> `ready_to_clear` 진입 -> named drain policy clear” 순서다.

## 비목표

- continuity owner commit
- cross-world migration 자체
- gameplay payload handoff
- autoscaling/orchestration mutation
