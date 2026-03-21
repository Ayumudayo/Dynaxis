# 확장성 제어면 API 계약

이 문서는 플러그인과 스크립트 배포를 위한 제어면(control plane) API 계약과 도메인 모델을 정의한다. `admin_app` 구현과 운영 절차가 어떤 약속 위에 서 있는지 설명하는 문서이며, 현재 구현된 `/api/v1/ext/*` 경로 동작을 기준으로 한다.

이 API를 별도 문서로 두는 이유는 명확하다. 확장 artifact 배포는 단순 파일 복사 문제가 아니라, "무엇을 배포할지", "어디에 배포할지", "충돌이 없는지", "지금 즉시 반영할지 예약할지"를 함께 다루는 운영 행위다. 이 계약이 명확하지 않으면 관리 콘솔, 자동화 스크립트, 운영자가 서로 다른 가정을 하게 되고, 그 결과 실제 배포보다 사전 검증과 롤백 판단이 더 어려워진다.

현재 이 문서가 특히 강조하는 원칙은 두 가지다.

- 조회와 적용을 분리한다.
  - inventory나 precheck는 "무슨 일이 일어날지"를 먼저 보여 준다.
- precheck와 deployment를 분리한다.
  - 검증 없이 바로 적용하면, 충돌이나 잘못된 대상 선택을 운영 중에야 발견하게 된다.

## 현재 구현 상태 (2026-03-06)

- `tools/admin_app/main.cpp`가 `inventory`, `precheck`, `deployments`, `schedules` 핵심 경로를 제공한다.
- precheck 충돌 차단(`exclusive_group_conflict`)과 deployment 상태 전이(`pending`, `precheck_passed`, `executing`, `completed`, `failed`)를 `admin_app` 기준으로 처리한다.
- 주요 회귀 검증은 `tests/python/verify_admin_api.py`에서 확인한다.

## 1) 기본 경로

- 기본 경로: `/api/v1/ext`
- `Content-Type`: `application/json`
- 시간 기준: UTC epoch milliseconds (`run_at_utc`)

시간 기준을 UTC epoch로 고정한 이유는, 예약 배포에서 가장 흔한 문제 중 하나가 로컬 시간대 해석 차이이기 때문이다. 운영자가 서로 다른 시간대에서 작업하거나, 스크립트가 다른 지역에서 실행될 때도 같은 시점을 가리켜야 예약과 감사가 일관된다.

## 2) 엔드포인트

### 2.1 `GET /api/v1/ext/inventory`

목적: 현재 배포 가능한 플러그인/스크립트 인벤토리를 조회한다.

이 조회가 필요한 이유는, 운영자가 배포 시점에 artifact 이름을 외워서 입력하게 두면 실수가 늘어나기 때문이다. inventory는 "무엇을 배포할 수 있는지"를 표준화된 메타데이터로 먼저 보여 주어, 배포 전 선택 오류를 줄인다.

쿼리 파라미터(선택):

- `kind`: `native_plugin` | `lua_script`
- `hook_scope`: `on_chat_send` | `on_login` | `on_join` | `on_leave` | `on_session_event` | `on_admin_command`
- `stage`: `pre_validate` | `mutate` | `authorize` | `side_effect` | `observe`
- `target_profile`: `chat` | `world` | `all`

응답 `200`:

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

inventory를 단순 파일 목록이 아니라 metadata 문서로 반환하는 이유는, 실제 운영 판단에 필요한 정보가 파일명보다 훨씬 많기 때문이다. `hook_scope`, `stage`, `exclusive_group`, `checksum`, `owner`가 있어야 충돌 검토와 변경 이력을 제대로 추적할 수 있다.

### 2.2 `POST /api/v1/ext/precheck`

목적: 충돌, 호환성, 대상 선택만 검증하고 실제 적용은 하지 않는다.

이 단계가 중요한 이유는, 운영에서 가장 값싼 실패는 "적용 전에 막히는 실패"이기 때문이다. precheck 없이 바로 배포를 만들면, 충돌이나 잘못된 대상 선택을 실행 단계에서야 발견하게 되고, 그때는 이미 일부 서버에 변화가 반영됐을 수 있다.

요청 본문:

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

응답 `200`:

```json
{
  "command_id": "cmd_20260305_001",
  "status": "precheck_passed",
  "target_count": 1,
  "issues": []
}
```

응답 `409` (precheck 실패):

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

`409`를 쓰는 이유도 의도적이다. 이는 서버 오류가 아니라 "현재 요청이 기존 운영 상태와 충돌한다"는 의미이므로, 호출자는 재시도보다 입력과 대상 선택을 다시 검토해야 한다.

### 2.3 `POST /api/v1/ext/deployments`

목적: 즉시 적용되는 배포를 생성한다.

이 엔드포인트는 "검증을 건너뛴 빠른 적용"을 위한 지름길이 아니라, precheck를 거친 뒤 실제 실행 단위를 만드는 경로로 읽어야 한다. 즉시 배포를 별도 리소스로 두는 이유는, 예약 배포와 달리 실행 시점이 곧바로 열리므로 상태 추적과 실패 분석을 더 자주 보게 되기 때문이다.

요청 본문:

- `run_at_utc`는 `null` 또는 생략
- 나머지 필드는 `POST /precheck`와 동일

응답 `202`:

```json
{
  "command_id": "cmd_20260305_001",
  "status": "pending"
}
```

`202`를 반환하는 이유는, 배포 생성과 실제 적용 완료가 같은 시점이 아니기 때문이다. 호출자는 "요청이 수락되었다"는 사실과 "이미 끝났다"를 구분해서 읽어야 한다.

### 2.4 `POST /api/v1/ext/schedules`

목적: 예약 배포를 생성한다.

예약 배포를 즉시 배포와 분리하는 이유는, 시간 조건이 들어가는 순간 검증 포인트가 달라지기 때문이다. clock skew 허용치, 예약 저장소, 나중에 실행될 command의 재평가 시점까지 같이 고려해야 한다.

요청 본문:

- `run_at_utc` 필수
- 나머지 필드는 `POST /precheck`와 동일

응답 `202`:

```json
{
  "command_id": "cmd_20260305_010",
  "status": "pending",
  "run_at_utc": 1770000000000
}
```

## 3) 배포 명령 DTO

필수 필드:

- `command_id`
  - 멱등성(idempotency)을 위한 키
- `artifact_id`
  - 배포 대상 artifact 식별자
- `selector`
  - 대상 서버 선택자
- `rollout_strategy`
  - 적용 전략

선택/조건부 필드:

- `run_at_utc`
  - 예약 배포일 때 필수, 즉시 배포는 `null` 또는 생략

`command_id`를 필수로 두는 이유는, 운영 자동화가 네트워크 오류 뒤에 같은 요청을 재시도할 때 중복 배포를 막기 위해서다. 멱등성 키가 없으면 "응답을 못 받았다"와 "실제로 배포가 안 됐다"를 구분하기 어려워져, 같은 artifact가 여러 번 배포될 수 있다.

### `selector`

`selector`는 배포 대상을 좁히는 문서다. 지원 필드는 다음과 같다.

- `all` (bool)
- `server_ids` (string[])
- `roles` (string[])
- `regions` (string[])
- `shards` (string[])
- `tags` (string[])

selector를 구조화된 필드로 강제하는 이유는, 운영자가 자유 텍스트 필터를 넣기 시작하면 나중에 감사, 재현, 자동화가 모두 어려워지기 때문이다. `roles`, `regions`, `shards`, `tags` 같은 기준을 분리해 두면 어떤 서버가 왜 선택됐는지 다시 설명하기 쉬워진다.

### `rollout_strategy`

지원 전략:

- `all_at_once`
- `canary_wave`

`canary_wave` 예시:

```json
{
  "type": "canary_wave",
  "waves": [5, 25, 100]
}
```

rollout 전략을 payload에 명시하는 이유는, "어떻게 배포할지"가 artifact 자체의 성질이 아니라 이번 운영 판단의 성질이기 때문이다. 같은 플러그인도 긴급 수정일 때와 신규 기능 실험일 때 rollout 방식이 달라질 수 있다.

## 4) 실행 상태 모델

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

상태를 세분화한 이유는 "배포가 성공/실패 둘 중 하나"라고만 보면 운영 해석이 너무 거칠어지기 때문이다. 예를 들어 `precheck_passed`는 검증은 끝났지만 아직 실행 전이라는 뜻이고, `executing`은 일부 반영이 시작되었을 수 있음을 뜻한다. 이 차이를 구분해야 취소 가능 시점과 장애 대응 시점을 올바르게 판단할 수 있다.

## 5) 설계 제약

- 같은 `command_id`의 재실행은 거부한다.
  - 중복 배포를 막기 위한 멱등성 규칙이다.
- precheck에 실패한 artifact는 deployment를 만들 수 없다.
  - "검증은 실패했지만 일단 적용해 보자" 같은 위험한 우회를 막는다.
- 충돌 판단은 `(hook_scope, stage, exclusive_group)` 조합 기준으로 한다.
  - 같은 위치에서 같은 역할을 하는 artifact가 동시에 활성화되는 것을 막는다.
- `observe` stage artifact는 상태 변경 결정을 반환하면 안 된다.
  - 관측과 상태 변경을 섞으면 운영자가 의도를 읽기 어려워지고, side effect 범위도 불명확해진다.

이 제약들은 모두 "운영자가 결과를 예측할 수 있게 한다"는 한 가지 목표를 향한다. 제어면은 기능을 많이 여는 것보다, 잘못된 조합을 미리 막는 편이 더 중요하다.

## 6) 함께 읽을 문서

- `docs/extensibility/overview.md`
- `docs/extensibility/conflict-policy.md`
- `docs/extensibility/recipes.md`

위 문서를 함께 읽어야 이 API가 단순 REST 표면이 아니라, 실제 런타임 배포 정책과 충돌 방지 규칙 위에서 동작한다는 점이 분명해진다.
