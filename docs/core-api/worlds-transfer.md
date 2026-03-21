# 월드 전환(World Transfer) API

`server/core/worlds/world_transfer.hpp`는 live world owner handoff 상태를 평가하는 stable 계약이다.

## 기준 이름

- 기준 include 경로: `server/core/worlds/world_transfer.hpp`
- 기준 네임스페이스: `server::core::worlds`

## 범위

- world lifecycle policy의 `draining + replacement_owner_instance_id` 상태를 operator-visible phase로 정규화한다
- current owner boundary와 replacement target readiness를 같은 모델에서 판단한다
- desired topology나 gameplay state migration payload는 이 surface에 포함하지 않는다

즉 이 계층은 “owner handoff가 지금 어느 단계에 있는가”를 평가하는 계약이지, 실제 handoff 실행 로직을 담은 계층이 아니다.

## 안정 타입

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

## 평가 규칙

`evaluate_world_transfer()`는 아래 우선순위로 phase를 산출한다.

1. transfer가 선언되지 않았으면 `idle`
2. replacement target이 inventory에 없으면 `target_missing`
3. replacement target이 ready가 아니면 `target_not_ready`
4. current owner boundary가 비어 있으면 `owner_missing`
5. current owner boundary가 replacement target과 같으면 `owner_handoff_committed`
6. 그 외에는 `awaiting_owner_handoff`

이 규칙이 필요한 이유는, 운영자가 “아직 대상 서버가 안 뜬 문제”와 “owner commit이 아직 안 된 문제”를 같은 장애로 보지 않게 하기 위해서다. phase를 분리해 두면 대응 우선순위를 더 명확히 잡을 수 있다.

## 관리자/운영자 매핑

현재 `admin_app`는 이 계약을 사용해 다음 응답의 `transfer` object를 구성한다.

- `GET /api/v1/worlds`
- `GET /api/v1/worlds/{world_id}/transfer`
- `PUT /api/v1/worlds/{world_id}/transfer`
- `DELETE /api/v1/worlds/{world_id}/transfer`

`PUT /transfer`는 world policy를 `draining=true` + replacement target으로 설정하고, `commit_owner=true`일 때 continuity world owner key도 same-world replacement owner로 커밋한다.

`DELETE /transfer`는 lifecycle policy만 해제하며, 이미 커밋된 owner boundary는 유지한다.

## 비목표

- active gameplay state migration
- automatic drain scheduler
- desired topology reconciliation 자체
- zone-to-zone payload handoff
