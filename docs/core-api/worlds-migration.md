# World Migration API

`server/core/worlds/migration.hpp`는 draining source world에서 target world owner로 넘어가는 migration envelope와 readiness/status evaluation contract를 정의한다.

## Canonical Naming

- canonical include path: `server/core/worlds/migration.hpp`
- canonical namespace: `server::core::worlds`
- `server/core/mmorpg/migration.hpp`는 compatibility wrapper로만 유지된다

## Scope

- source world drain과 분리된 named migration envelope를 표현한다
- target world / target owner / preserve-room intent와 app-defined payload reference를 운반한다
- gameplay state payload 자체는 해석하지 않고 opaque reference로만 취급한다

## Stable Types

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
- helper:
  - `evaluate_world_migration(...)`
  - `parse_world_migration_envelope(...)`
  - `serialize_world_migration_envelope(...)`

## Evaluation Rule

`evaluate_world_migration()`는 아래 우선순위로 phase를 정한다.

1. envelope가 없으면 `idle`
2. target world가 observed inventory에 없으면 `target_world_missing`
3. target owner가 target world inventory에 없으면 `target_owner_missing`
4. target owner가 ready가 아니면 `target_owner_not_ready`
5. source world가 아직 draining이 아니면 `awaiting_source_drain`
6. 그 외에는 `ready_to_resume`

## Current Runtime Mapping

현재 contract는 두 곳에서 사용된다.

- `admin_app`
  - `GET/PUT/DELETE /api/v1/worlds/{world_id}/migration`
  - `GET /api/v1/worlds`
  - `GET /api/v1/topology/observed`
- `server_app` continuity resume
  - source world가 draining이고 migration envelope target owner가 현재 backend와 맞으면 target world로 restore를 시도한다
  - `preserve_room=true`이면 app-local room continuity도 함께 복원한다
  - `payload_kind="chat-room-v1"`이면 `payload_ref`를 app-local target room으로 해석해 generic preserve-room restore 대신 그 방으로 handoff한다

## Non-Goals

- desired topology reconciliation
- same-world owner handoff
- autoscaling
- gameplay payload schema
- combat/simulation state replication

