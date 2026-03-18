# MMORPG World Transfer API

`server/core/mmorpg/world_transfer.hpp`는 live world owner handoff 상태를 평가하는 stable contract다.

## Scope

- world lifecycle policy의 `draining + replacement_owner_instance_id` 상태를 operator-visible phase로 정규화한다
- current owner boundary와 replacement target readiness를 한 모델에서 판단한다
- desired topology와 gameplay state migration payload는 이 surface에 포함하지 않는다

## Stable Types

- `ObservedWorldTransferInstance`
  - same-world `server` instance readiness view
- `ObservedWorldTransferState`
  - `world_id`
  - current `owner_instance_id`
  - `draining`
  - `replacement_owner_instance_id`
  - `instances[]`
- `WorldTransferPhase`
  - `idle`
  - `target_missing`
  - `target_not_ready`
  - `owner_missing`
  - `awaiting_owner_handoff`
  - `owner_handoff_committed`
- `WorldTransferSummary`
- `WorldTransferStatus`

## Evaluation Rule

`evaluate_world_transfer()`는 아래 우선순위로 phase를 산출한다.

1. transfer가 선언되지 않았으면 `idle`
2. replacement target이 inventory에 없으면 `target_missing`
3. replacement target이 ready가 아니면 `target_not_ready`
4. current owner boundary가 비어 있으면 `owner_missing`
5. current owner boundary가 replacement target과 같으면 `owner_handoff_committed`
6. 그 외에는 `awaiting_owner_handoff`

## Admin/Operator Mapping

현재 `admin_app`는 이 contract를 사용해:

- `GET /api/v1/worlds`
- `GET /api/v1/worlds/{world_id}/transfer`
- `PUT /api/v1/worlds/{world_id}/transfer`
- `DELETE /api/v1/worlds/{world_id}/transfer`

응답의 `transfer` object를 구성한다.

`PUT /transfer`는 world policy를 `draining=true` + replacement target으로 설정하고,
`commit_owner=true`일 때 continuity world owner key도 same-world replacement owner로 커밋한다.

`DELETE /transfer`는 lifecycle policy만 해제하며, 이미 커밋된 owner boundary는 유지한다.

## Non-Goals

- active gameplay state migration
- automatic drain scheduler
- desired topology reconciliation 자체
- zone-to-zone payload handoff
