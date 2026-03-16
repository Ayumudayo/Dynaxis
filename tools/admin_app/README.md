# 관리자 앱(admin_app)

`admin_app`은 운영 관리 콘솔을 위한 제어면(control-plane) 서비스다.

현재 구현된 API/권한/운영 surface의 canonical 문서는 이 README다.

현재 제공 엔드포인트:

- `/metrics`
- `/healthz`, `/readyz`
- `/admin` (브라우저 UI, 소스(source): `tools/admin_app/admin_ui.html`)
- `/api/v1/auth/context` (JSON)
- `/api/v1/overview` (JSON)
- `/api/v1/instances` (JSON)
- `/api/v1/instances/{instance_id}` (JSON)
- `/api/v1/worlds` (JSON)
- `/api/v1/worlds/{world_id}/policy` (PUT/DELETE, JSON)
- `/api/v1/sessions/{client_id}` (JSON)
- `/api/v1/users` (JSON)
- `/api/v1/users/disconnect` (POST, query/body)
- `/api/v1/users/mute` (POST, query/body)
- `/api/v1/users/unmute` (POST, query/body)
- `/api/v1/users/ban` (POST, query/body)
- `/api/v1/users/unban` (POST, query/body)
- `/api/v1/users/kick` (POST, query/body)
- `/api/v1/announcements` (POST, query/body)
- `/api/v1/settings` (PATCH, query/body)
- `/api/v1/ext/inventory` (GET)
- `/api/v1/ext/precheck` (POST, JSON)
- `/api/v1/ext/deployments` (GET/POST, JSON)
- `/api/v1/ext/schedules` (POST, JSON)
- `/api/v1/worker/write-behind` (JSON)
- `/api/v1/metrics/links` (JSON)

## 환경 변수

| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `METRICS_PORT` | 관리자 HTTP 포트 | `39200` |
| `ADMIN_POLL_INTERVAL_MS` | 백그라운드 상태 수집 주기(ms) | `1000` |
| `ADMIN_AUDIT_TREND_MAX_POINTS` | overview `audit_trend` 히스토리 최대 포인트 수 | `300` |
| `ADMIN_INSTANCE_METRICS_PORT` | 인스턴스 상세 조회 시 metrics/ready probe 포트 | `9090` |
| `REDIS_URI` | Redis 연결 문자열(인스턴스/세션 조회용) | (unset) |
| `SERVER_REGISTRY_PREFIX` | 인스턴스 레지스트리 접두어(prefix) | `gateway/instances/` |
| `GATEWAY_SESSION_PREFIX` | 세션 디렉터리 키(key) 접두어(prefix) | `gateway/session/` |
| `REDIS_CHANNEL_PREFIX` | fanout/admin 명령 채널 접두어(prefix) | `` |
| `SESSION_CONTINUITY_REDIS_PREFIX` | continuity world owner key 조회용 접두어(prefix). 미설정이면 `REDIS_CHANNEL_PREFIX`를 재사용 | (unset) |
| `WB_WORKER_METRICS_URL` | wb_worker 메트릭 URL | `http://127.0.0.1:39093/metrics` |
| `GRAFANA_BASE_URL` | Grafana 기본 URL 링크 | `http://127.0.0.1:33000` |
| `PROMETHEUS_BASE_URL` | Prometheus 기본 URL 링크 | `http://127.0.0.1:39090` |
| `ADMIN_AUTH_MODE` | 인증 모드 (`off`, `header`, `bearer`, `header_or_bearer`) | `off` |
| `ADMIN_READ_ONLY` | write endpoint 킬스위치(`1`이면 write 요청 차단) | `0` |
| `ADMIN_COMMAND_SIGNING_SECRET` | admin fanout 명령 payload HMAC 서명 키(미설정 시 write publish 차단) | (unset) |
| `ADMIN_OFF_ROLE` | `ADMIN_AUTH_MODE=off`일 때 적용할 역할(role) (`viewer/operator/admin`) | `admin` |
| `ADMIN_AUTH_USER_HEADER` | header 인증 시 사용자 헤더(header) 이름 | `X-Admin-User` |
| `ADMIN_AUTH_ROLE_HEADER` | header 인증 시 역할 헤더(header) 이름 | `X-Admin-Role` |
| `ADMIN_BEARER_TOKEN` | bearer 인증 토큰 값 | (unset) |
| `ADMIN_BEARER_ACTOR` | bearer 인증 성공 시 주체(actor) 값 | `token-user` |
| `ADMIN_BEARER_ROLE` | bearer 인증 성공 시 역할(role) 값 (`viewer/operator/admin`) | `viewer` |
| `ADMIN_EXT_SCHEDULE_STORE_PATH` | 확장 배포 schedule 저장소(JSON) 경로 | `tasks/runtime_ext_deployments_store.json` |
| `ADMIN_EXT_MAX_CLOCK_SKEW_MS` | 예약 실행 허용 clock skew(ms) | `5000` |
| `ADMIN_EXT_FORCE_FAIL_WAVE_INDEX` | (테스트용) 해당 wave에서 강제 실패(0=비활성) | `0` |
| `METRICS_HTTP_MAX_CONNECTIONS` | admin/metrics HTTP 동시 연결 상한 | `64` |
| `METRICS_HTTP_HEADER_TIMEOUT_MS` | admin/metrics HTTP header read timeout(ms) | `5000` |
| `METRICS_HTTP_BODY_TIMEOUT_MS` | admin/metrics HTTP body read timeout(ms) | `5000` |
| `METRICS_HTTP_MAX_BODY_BYTES` | admin/metrics HTTP body 최대 크기(바이트) | `65536` |
| `METRICS_HTTP_AUTH_TOKEN` | 설정 시 bearer 또는 `X-Metrics-Token` 인증 강제 | (unset) |
| `METRICS_HTTP_ALLOWLIST` | 콤마 구분 source IP allowlist | (unset) |

## 빌드

```powershell
pwsh scripts/build.ps1 -Config Debug -Target admin_app
```

## 실행

```powershell
set METRICS_PORT=39200
.\build-windows\Debug\admin_app.exe
```

브라우저 접속:

- `http://127.0.0.1:39200/admin`

## 도커(Docker) 스택

`docker/stack`에 `admin-app` 서비스가 포함된다.

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability
```

브라우저 접속:

- `http://127.0.0.1:39200/admin`

## 참고 문서

- `docs/ops/observability.md`

## 공통 쿼리 파라미터

`/api/v1/*`는 다음 쿼리 파라미터를 지원한다.

- `timeout_ms` (선택, 최대 `5000`)
- `limit` (선택, 최대 `500`)
- `cursor` (선택)

## 인스턴스 world scope visibility

- `GET /api/v1/instances`
- `GET /api/v1/instances/{instance_id}`

server 인스턴스는 `world_scope`를 함께 반환한다.

- `world_scope.world_id`
- `world_scope.owner_instance_id`
- `world_scope.owner_match`
- `world_scope.policy.draining`
- `world_scope.policy.replacement_owner_instance_id`
- `world_scope.policy.reassignment_declared`
- `world_scope.source.owner_key`
- `world_scope.source.policy_key`

이 값은 `world:<id>` tag와 continuity owner key를 조합해, 현재 backend가 해당 world residency owner와 일치하는지 control-plane에서 바로 확인하게 한다.

## World lifecycle inventory

- `GET /api/v1/worlds`

world 단위의 owner/policy 인벤토리를 반환한다.

- `world_id`
- `owner_instance_id`
- `policy.draining`
- `policy.replacement_owner_instance_id`
- `policy.reassignment_declared`
- `instances[]` (`instance_id`, `ready`, `owner_match`)
- `source.owner_key`
- `source.policy_key`

## 쓰기 액션 (2단계)

`admin_app`은 아래 쓰기 엔드포인트를 제공한다. `MetricsHttpServer`는 `Content-Length` 기반 body read를 지원하므로,
운영에서는 query 파라미터보다 JSON body 사용을 권장한다. (기존 query 호출은 호환 유지)

`ADMIN_READ_ONLY=1`이면 아래 write 엔드포인트는 역할과 무관하게 `403` + `READ_ONLY`로 거부된다.
`/api/v1/auth/context`의 `data.read_only`는 `true`가 되고, write capability(`disconnect/announce/settings/moderation/world_policy`)는 모두 `false`로 내려간다.

`ADMIN_COMMAND_SIGNING_SECRET`이 설정되면 write 명령 publish payload에
`issued_at`, `nonce`, `signature`(HMAC-SHA256)가 자동 추가된다.
미설정 상태에서는 write 명령 publish가 `503` + `MISCONFIGURED`로 거부된다.

world lifecycle policy write는 예외다.

- `PUT/DELETE /api/v1/worlds/{world_id}/policy` 는 fanout/admin 명령 publish를 사용하지 않는다.
- authoritative state는 Redis `dynaxis:continuity:world-policy:<world_id>` key 자체다.
- 따라서 이 endpoint는 `ADMIN_COMMAND_SIGNING_SECRET` 없이도 동작한다.

- `POST /api/v1/users/disconnect`
  - `client_id` 또는 `client_ids`(쉼표 구분)
  - `reason` (선택)
- `POST /api/v1/users/mute`
- `POST /api/v1/users/unmute`
- `POST /api/v1/users/ban`
- `POST /api/v1/users/unban`
- `POST /api/v1/users/kick`
  - 공통: `client_id` 또는 `client_ids`(쉼표 구분)
  - 공통: `reason` (선택)
  - `mute`/`ban`은 `duration_sec` (선택)
- `POST /api/v1/announcements`
  - `text` (필수, 최대 512 bytes)
  - `priority` (`info|warn|critical`, 선택)
- `PATCH /api/v1/settings`
  - `key` (`presence_ttl_sec|recent_history_limit|room_recent_maxlen|chat_spam_threshold|chat_spam_window_sec|chat_spam_mute_sec|chat_spam_ban_sec|chat_spam_ban_violations`)
  - `value` (부호 없는 정수)
- `PUT /api/v1/worlds/{world_id}/policy`
  - JSON body required
  - `draining` (필수, bool)
  - `replacement_owner_instance_id` (선택, string 또는 `null`)
  - `replacement_owner_instance_id`는 현재 world inventory에 존재하는 같은 world의 `server` 인스턴스여야 한다
- `DELETE /api/v1/worlds/{world_id}/policy`
  - 해당 world policy key를 제거한다
  - key가 이미 없어도 성공으로 처리한다

설정 key/range 검증은 `server/include/server/config/runtime_settings.hpp`의 공통 규칙을 사용한다.
동일 규칙이 `admin_app` 입력 검증과 `server_app` 런타임 적용 경로에 함께 적용되어 문서/코드 드리프트(drift)를 줄인다.

권한:

- `viewer`: 조회 전용(read-only)
- `operator`: 공지(announcement) 가능
- `admin`: runtime settings + moderation + world lifecycle policy 포함 전체 가능

## 확장 배포 제어면 (Phase 7)

`/api/v1/ext/*`는 플러그인/스크립트 배포 제어를 위한 API다.

- `GET /api/v1/ext/inventory`
  - manifest 인벤토리 조회 (`kind`, `hook_scope`, `stage`, `target_profile` 필터 지원)
- `POST /api/v1/ext/precheck`
  - 적용 없는 사전 검증
  - 충돌 정책 `(hook_scope, stage, exclusive_group)` 검사
  - `observe` stage decision 제한 검사
  - 실패 시 `409 PRECHECK_FAILED` + 상세 `issues` 반환
- `POST /api/v1/ext/deployments`
  - 즉시 배포 생성 (멱등 키 `command_id` 중복 거부)
  - 기본 전략: `all_at_once`
  - `canary_wave` 사용 시 기본 wave: `5,25,100`
- `POST /api/v1/ext/schedules`
  - 예약 배포 생성 (`run_at_utc` 필수)
  - scheduler가 UTC 기준으로 실행
  - `ADMIN_EXT_MAX_CLOCK_SKEW_MS` 초과 시 `failed(clock_skew)` 처리

`command_id`는 실행 멱등 키이며, 이미 생성된 배포의 동일 `command_id` 재사용은 `409 IDEMPOTENT_REJECTED`로 거부된다.

`ADMIN_READ_ONLY=1`일 때도 `POST /api/v1/ext/precheck`는 허용되며,
실제 변경을 일으키는 `POST /api/v1/ext/deployments`, `POST /api/v1/ext/schedules`는 차단된다.

## 감사 로그

`/admin`, `/api/v1/*` 요청은 `admin_audit` 구조화 로그를 남긴다.

- `request_id`, `actor`, `role`
- `method`, `path`, `resource`
- `result`, `status_code`, `latency_ms`
- `source_ip`, `timestamp`

## 개요(Overview) 트렌드

`GET /api/v1/overview`는 `counts` 외에 `audit_trend`를 제공한다.

- `counts.http_server_errors_total`
- `audit_trend.step_ms`, `audit_trend.max_points`
- `audit_trend.points[]` (`timestamp_ms`, `http_errors_total`, `http_server_errors_total`, `http_unauthorized_total`, `http_forbidden_total`)
