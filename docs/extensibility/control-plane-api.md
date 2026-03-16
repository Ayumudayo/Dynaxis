# Extensibility Control Plane API Contract

이 문서는 플러그인/스크립트 배포 제어면의 API 계약과 도메인 모델을 정의한다.
관리 콘솔/서버 구현과 정렬되는 운영 계약 문서이며, 현재 구현된 `/api/v1/ext/*` 경로 동작 기준을 정의한다.

## 현재 구현 상태 (2026-03-06)

- `tools/admin_app/main.cpp`에서 `inventory/precheck/deployments/schedules` 핵심 경로를 제공한다.
- precheck 충돌 차단(`exclusive_group_conflict`)과 deployment 상태 전이(`pending/precheck_passed/executing/completed/failed`)를 admin-app 기준으로 처리한다.
- 회귀 검증은 `tests/python/verify_admin_api.py`에서 주요 경로를 확인한다.

## 1) Base Path

- Base: `/api/v1/ext`
- Content-Type: `application/json`
- 시간 기준: UTC epoch milliseconds (`run_at_utc`)

## 2) Endpoints

### 2.1 GET `/api/v1/ext/inventory`

목적: 배포 가능한 플러그인/스크립트 인벤토리 조회.

Query (optional):
- `kind`: `native_plugin` | `lua_script`
- `hook_scope`: `on_chat_send` | `on_login` | `on_join` | `on_leave` | `on_session_event` | `on_admin_command`
- `stage`: `pre_validate` | `mutate` | `authorize` | `side_effect` | `observe`
- `target_profile`: `chat` | `world` | `all`

Response 200:

```json
{
  "items": [
    {
      "artifact_id": "plugin:30_spam_filter",
      "kind": "native_plugin",
      "name": "spam_filter",
      "version": "1.2.0",
      "hook_scope": ["on_chat_send", "on_join"],
      "stage": "mutate",
      "priority": 30,
      "exclusive_group": "spam_filter",
      "checksum": "sha256:...",
      "target_profiles": ["all"],
      "owner": "chat-ops"
    }
  ]
}
```

### 2.2 POST `/api/v1/ext/precheck`

목적: 충돌/호환/대상 검증만 수행(적용 없음).

Request body:

```json
{
  "command_id": "cmd_20260305_001",
  "artifact_id": "plugin:30_spam_filter",
  "selector": {
    "all": false,
    "server_ids": ["server-a-01"],
    "roles": ["chat"],
    "regions": ["ap-northeast"],
    "shards": ["shard-01"],
    "tags": ["canary"]
  },
  "run_at_utc": null,
  "rollout_strategy": {
    "type": "all_at_once"
  }
}
```

Response 200:

```json
{
  "command_id": "cmd_20260305_001",
  "status": "precheck_passed",
  "target_count": 1,
  "issues": []
}
```

Response 409 (precheck failure):

```json
{
  "command_id": "cmd_20260305_001",
  "status": "failed",
  "issues": [
    {
      "code": "exclusive_group_conflict",
      "message": "hook_scope=on_join, stage=authorize, exclusive_group=vip_gate already active"
    }
  ]
}
```

### 2.3 POST `/api/v1/ext/deployments`

목적: 즉시 적용 배포 생성.

Request body:
- `run_at_utc`는 `null` 또는 omitted (즉시 적용)
- 그 외 필드는 `POST /precheck`와 동일

Response 202:

```json
{
  "command_id": "cmd_20260305_001",
  "status": "pending"
}
```

### 2.4 POST `/api/v1/ext/schedules`

목적: 예약 배포 생성.

Request body:
- `run_at_utc` 필수
- 그 외 필드는 `POST /precheck`와 동일

Response 202:

```json
{
  "command_id": "cmd_20260305_010",
  "status": "pending",
  "run_at_utc": 1770000000000
}
```

## 3) Deployment Command DTO

필수 필드:
- `command_id`: 멱등성 키
- `artifact_id`: 배포 대상 artifact 식별자
- `selector`: 대상 서버 선택자
- `rollout_strategy`: 적용 전략

선택/조건부 필드:
- `run_at_utc`: 예약 배포 시 필수, 즉시 배포는 `null`/생략

`selector`:
- `all` (bool)
- `server_ids` (string[])
- `roles` (string[])
- `regions` (string[])
- `shards` (string[])
- `tags` (string[])

`rollout_strategy`:
- `all_at_once`
- `canary_wave`

`canary_wave` 파라미터 예시:

```json
{
  "type": "canary_wave",
  "waves": [5, 25, 100]
}
```

## 4) Execution Status Model

허용 상태:
- `pending`
- `precheck_passed`
- `executing`
- `completed`
- `failed`
- `cancelled`

전이 규칙:
- `pending -> precheck_passed -> executing -> completed`
- 실패 시 `pending|precheck_passed|executing -> failed`
- 운영자 취소 시 `pending|precheck_passed -> cancelled`

## 5) Design Constraints

- 동일 `command_id` 재실행은 거부(idempotent).
- precheck 실패 artifact는 배포 생성 차단.
- 충돌 판단은 `(hook_scope, stage, exclusive_group)` 조합 기준.
- `observe` stage artifact는 상태 변경 결정을 반환하면 precheck 실패.

## 6) Related Documents

- `docs/extensibility/overview.md`
- `docs/extensibility/conflict-policy.md`
- `docs/extensibility/recipes.md`
