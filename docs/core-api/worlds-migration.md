# 월드 마이그레이션(World Migration) API

`server/core/worlds/migration.hpp`는 draining source world에서 target world owner로 넘어갈 때 필요한 migration envelope와 readiness/status evaluation 계약을 정의한다.

## 기준 이름

- 기준 include 경로: `server/core/worlds/migration.hpp`
- 기준 네임스페이스: `server::core::worlds`

## 범위

- source world drain과 분리된 named migration envelope를 표현한다
- target world / target owner / preserve-room intent와 app-defined payload reference를 함께 운반한다
- gameplay state payload 자체는 해석하지 않고 opaque reference로만 취급한다

이렇게 분리하는 이유는 “마이그레이션 의도”와 “도메인 payload 해석”을 같은 계층에 두지 않기 위해서다. 둘을 섞으면 core contract가 특정 게임/채팅 payload 의미에 종속돼 재사용성이 크게 떨어진다.

## 안정 타입

- `WorldMigrationEnvelope`
  - `target_world_id`
  - `target_owner_instance_id`
  - `preserve_room`
  - `payload_kind`
  - `payload_ref`
  - `updated_at_ms`
- `ObservedWorldMigrationInstance`
- `ObservedWorldMigrationWorld`
- `WorldMigrationPhase`
  - `idle`
  - `target_world_missing`
  - `target_owner_missing`
  - `target_owner_not_ready`
  - `awaiting_source_drain`
  - `ready_to_resume`
- `WorldMigrationSummary`
- `WorldMigrationStatus`
- helper
  - `evaluate_world_migration(...)`
  - `parse_world_migration_envelope(...)`
  - `serialize_world_migration_envelope(...)`

## 평가 규칙

`evaluate_world_migration()`는 아래 우선순위로 phase를 정한다.

1. envelope가 없으면 `idle`
2. target world가 observed inventory에 없으면 `target_world_missing`
3. target owner가 target world inventory에 없으면 `target_owner_missing`
4. target owner가 ready가 아니면 `target_owner_not_ready`
5. source world가 아직 draining이 아니면 `awaiting_source_drain`
6. 그 외에는 `ready_to_resume`

phase를 이렇게 나누는 이유는, 운영자가 “대상 월드가 아직 안 뜬 문제”, “대상 owner가 불안정한 문제”, “source drain이 아직 끝나지 않은 문제”를 서로 다른 원인으로 바로 구분할 수 있게 하기 위해서다.

## 현재 런타임 매핑

현재 이 계약은 두 곳에서 주로 사용된다.

- `admin_app`
  - `GET/PUT/DELETE /api/v1/worlds/{world_id}/migration`
  - `GET /api/v1/worlds`
  - `GET /api/v1/topology/observed`
- `server_app` continuity resume
  - source world가 draining이고 migration envelope의 target owner가 현재 backend와 맞으면 target world로 restore를 시도한다
  - `preserve_room=true`이면 app-local room continuity도 함께 복원한다
  - `payload_kind="chat-room-v1"`이면 `payload_ref`를 app-local target room으로 해석해 generic preserve-room restore 대신 그 방으로 handoff한다

## 비목표

- desired topology reconciliation 자체
- same-world owner handoff
- autoscaling
- gameplay payload schema
- combat/simulation state replication
