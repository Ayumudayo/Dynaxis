# 관리자 앱(admin_app)

`admin_app`은 운영 관리 콘솔을 위한 제어면(control plane) 서비스다.

현재 구현된 API, 권한, 운영 표면의 기준 문서는 이 README다.

이 프로세스를 별도로 두는 이유는 중요하다. 운영 읽기와 쓰기 액션을 `server_app` 본체 안에 섞어 두면, 제어면 문제와 데이터면(data plane) 문제를 분리하기 어렵다. `admin_app`을 따로 두면 다음이 쉬워진다.

- 운영 조회를 본체 트래픽과 분리한다
- world drain, transfer, migration 같은 제어 명령을 한곳에서 관리한다
- topology 문서와 실행 상태를 단계별로 관찰한다
- 권한, read-only, 감사 로그를 별도 정책으로 묶는다

초보자는 이 문서를 아래 순서로 읽는 편이 좋다.

1. 상단 엔드포인트 목록
   - 어떤 기능 군이 있는지 큰 그림을 본다
2. 환경 변수
   - 어떤 외부 의존성과 보안 스위치가 필요한지 확인한다
3. 쓰기 액션
   - 실제 운영 명령이 어떤 보호 장치 아래 있는지 본다
4. 토폴로지 계층
   - desired -> observed -> reconciliation -> actuation request/execution/realization 순서를 이해한다

특히 토폴로지 계층은 일부러 여러 단계로 나뉘어 있다. 이 분리가 없으면 "운영자가 원한 상태", "실제로 본 상태", "실행기가 처리 중인 상태"가 한 문서에 뒤섞여, 장애 시 무엇이 진실인지 판단하기 어려워진다.

현재 제공 엔드포인트는 크게 다섯 부류로 보면 읽기 쉽다.

- 기본 운영 표면
  - `/metrics`, `/healthz`, `/readyz`, `/admin`
- 인벤토리/조회
  - 인스턴스, 세션, 사용자, world 상태를 읽는다
- 쓰기 액션
  - disconnect, moderation, announcement, runtime setting, world policy 변경을 수행한다
- 토폴로지 제어
  - desired, observed, reconciliation, actuation request/execution/realization을 단계별로 다룬다
- 확장 배포
  - plugin/script inventory, precheck, deployment, schedule을 다룬다

현재 제공 엔드포인트:

- `/metrics`
- `/healthz`, `/readyz`
- `/admin` (브라우저 UI, 소스: `tools/admin_app/admin_ui.html`)
- `/api/v1/auth/context` (JSON)
- `/api/v1/overview` (JSON)
- `/api/v1/instances` (JSON)
- `/api/v1/instances/{instance_id}` (JSON)
- `/api/v1/worlds` (JSON)
- `/api/v1/worlds/{world_id}/policy` (PUT/DELETE, JSON)
- `/api/v1/worlds/{world_id}/drain` (GET/PUT/DELETE, JSON)
- `/api/v1/worlds/{world_id}/transfer` (GET/PUT/DELETE, JSON)
- `/api/v1/worlds/{world_id}/migration` (GET/PUT/DELETE, JSON)
- `/api/v1/topology/desired` (GET/PUT/DELETE, JSON)
- `/api/v1/topology/observed` (JSON)
- `/api/v1/topology/reconciliation` (JSON)
- `/api/v1/topology/actuation` (JSON)
- `/api/v1/topology/actuation/request` (GET/PUT/DELETE, JSON)
- `/api/v1/topology/actuation/status` (JSON)
- `/api/v1/topology/actuation/execution` (GET/PUT/DELETE, JSON)
- `/api/v1/topology/actuation/execution/status` (JSON)
- `/api/v1/topology/actuation/realization` (JSON)
- `/api/v1/topology/actuation/adapter` (GET/PUT/DELETE, JSON)
- `/api/v1/topology/actuation/adapter/status` (JSON)
- `/api/v1/topology/actuation/runtime-assignment` (GET/PUT/DELETE, JSON)
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

이 섹션은 단순한 실행 옵션 목록이 아니다. `admin_app`은 Redis 조회, write-behind 워커 메트릭 수집, 인증, read-only 보호, 토폴로지 문서 저장을 모두 다루므로, 어떤 변수가 "기능 토글"인지 "보안 장치"인지 구분해 보는 편이 좋다.

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
| `SESSION_CONTINUITY_LEASE_TTL_SEC` | owner transfer commit 시 continuity world owner key에 적용할 TTL(초) | `900` |
| `WB_WORKER_METRICS_URL` | wb_worker 메트릭 URL | `http://127.0.0.1:39093/metrics` |
| `GRAFANA_BASE_URL` | Grafana 기본 URL 링크 | `http://127.0.0.1:33000` |
| `PROMETHEUS_BASE_URL` | Prometheus 기본 URL 링크 | `http://127.0.0.1:39090` |
| `ADMIN_AUTH_MODE` | 인증 모드 (`off`, `header`, `bearer`, `header_or_bearer`) | `off` |
| `ADMIN_READ_ONLY` | 쓰기 엔드포인트 킬스위치(`1`이면 write 요청 차단) | `0` |
| `ADMIN_COMMAND_SIGNING_SECRET` | admin fanout 명령 payload HMAC 서명 키(미설정 시 write publish 차단) | (unset) |
| `ADMIN_DESIRED_TOPOLOGY_KEY` | desired topology document Redis key | `dynaxis:topology:desired` |
| `ADMIN_TOPOLOGY_ACTUATION_REQUEST_KEY` | topology actuation request document Redis key | `dynaxis:topology:actuation:request` |
| `ADMIN_TOPOLOGY_ACTUATION_EXECUTION_KEY` | topology actuation execution progress document Redis key | `dynaxis:topology:actuation:execution` |
| `ADMIN_TOPOLOGY_ACTUATION_ADAPTER_KEY` | topology actuation adapter lease document Redis key | `dynaxis:topology:actuation:adapter` |
| `ADMIN_TOPOLOGY_ACTUATION_RUNTIME_ASSIGNMENT_KEY` | topology actuation runtime-assignment document Redis key | `dynaxis:topology:actuation:runtime-assignment` |
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

## 도커 스택

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

## 인스턴스 world 범위 가시성

- `GET /api/v1/instances`
- `GET /api/v1/instances/{instance_id}`

이 조회가 필요한 이유는 단순 인스턴스 목록보다 "현재 backend가 어떤 world 책임을 실제로 지고 있는가"가 운영에서 더 중요하기 때문이다. instance는 살아 있어 보여도 잘못된 world owner를 보고 있을 수 있으므로, `world_scope`는 단순 부가 정보가 아니라 소유권 검증용 읽기 표면이다.

server 인스턴스는 `world_scope`를 함께 반환한다.

- `world_scope.world_id`
- `world_scope.owner_instance_id`
- `world_scope.owner_match`
- `world_scope.policy.draining`
- `world_scope.policy.replacement_owner_instance_id`
- `world_scope.policy.reassignment_declared`
- `world_scope.source.owner_key`
- `world_scope.source.policy_key`

이 값은 `world:<id>` tag와 continuity owner key를 조합해, 현재 backend가 해당 world residency owner와 일치하는지 제어면에서 바로 확인하게 한다.

## world 수명주기 인벤토리

- `GET /api/v1/worlds`

이 엔드포인트는 world 단위의 상태를 한곳에서 모아 보여 주는 요약 읽기 모델이다. 운영자가 drain, transfer, migration을 각각 다른 키와 로그에서 따로 추적하게 두면 판단이 느려지고 실수도 늘어난다. 그래서 owner, policy, orchestration summary를 한 문서로 묶어 "지금 world가 어떤 전환 단계에 있는가"를 먼저 보게 만든다.

world 단위의 owner/policy 인벤토리를 반환한다.

- `world_id`
- `owner_instance_id`
- `policy.draining`
- `policy.replacement_owner_instance_id`
- `policy.reassignment_declared`
- `drain.phase`
- `drain.current_owner_instance_id`
- `drain.replacement_owner_instance_id`
- `drain.summary.drain_declared`
- `drain.summary.replacement_declared`
- `drain.summary.replacement_present`
- `drain.summary.replacement_ready`
- `drain.summary.instances_total`
- `drain.summary.ready_instances`
- `drain.summary.active_sessions_total`
- `drain.summary.owner_active_sessions`
- `drain.summary.replacement_active_sessions`
- `drain.orchestration.phase`
- `drain.orchestration.next_action`
- `drain.orchestration.target_owner_instance_id`
- `drain.orchestration.target_world_id`
- `drain.orchestration.summary.drain_declared`
- `drain.orchestration.summary.drained`
- `drain.orchestration.summary.transfer_declared`
- `drain.orchestration.summary.transfer_committed`
- `drain.orchestration.summary.migration_declared`
- `drain.orchestration.summary.migration_ready`
- `drain.orchestration.summary.clear_allowed`
- `transfer.phase`
- `transfer.current_owner_instance_id`
- `transfer.target_owner_instance_id`
- `transfer.summary.transfer_declared`
- `transfer.summary.owner_present`
- `transfer.summary.target_present`
- `transfer.summary.target_ready`
- `transfer.summary.owner_matches_target`
- `migration.phase`
- `migration.target_world_id`
- `migration.target_owner_instance_id`
- `migration.payload_kind`
- `migration.payload_ref`
- `migration.summary.envelope_present`
- `migration.summary.source_draining`
- `migration.summary.target_world_present`
- `migration.summary.target_owner_present`
- `migration.summary.target_owner_ready`
- `migration.summary.preserve_room`
- `instances[]` (`instance_id`, `ready`, `owner_match`)
- `source.owner_key`
- `source.policy_key`
- `source.migration_key`

## 쓰기 액션 (2단계)

`admin_app`은 아래 쓰기 엔드포인트를 제공한다. `MetricsHttpServer`는 `Content-Length` 기반 body read를 지원하므로,
운영에서는 query 파라미터보다 JSON body 사용을 권장한다. (기존 query 호출은 호환 유지)

이 섹션에서 가장 중요한 점은 "admin_app이 아무 때나 곧바로 쓰지 않는다"는 것이다. read-only 스위치, 역할(role), 서명(secret), 그리고 일부 world/topology 문서의 직접 저장 경계를 분리해 두는 이유는, 운영자가 잘못된 명령 한 번으로 runtime 전체 상태를 망치지 않게 하기 위해서다.

`ADMIN_READ_ONLY=1`이면 아래 쓰기 엔드포인트는 역할과 무관하게 `403` + `READ_ONLY`로 거부된다.
`/api/v1/auth/context`의 `data.read_only`는 `true`가 되고, write capability(`disconnect/announce/settings/moderation/world_policy/world_drain/world_transfer/world_migration/desired_topology/topology_actuation_request/topology_actuation_execution/topology_actuation_adapter/topology_actuation_runtime_assignment`)는 모두 `false`로 내려간다.

`ADMIN_COMMAND_SIGNING_SECRET`이 설정되면 write 명령 publish payload에
`issued_at`, `nonce`, `signature`(HMAC-SHA256)가 자동 추가된다.
미설정 상태에서는 write 명령 publish가 `503` + `MISCONFIGURED`로 거부된다.

world lifecycle policy / drain / transfer / migration write는 예외다.

- `PUT/DELETE /api/v1/worlds/{world_id}/policy` 는 fanout/admin 명령 publish를 사용하지 않는다.
- authoritative state는 Redis `dynaxis:continuity:world-policy:<world_id>` key 자체다.
- `GET/PUT/DELETE /api/v1/worlds/{world_id}/transfer` 도 fanout/admin 명령 publish를 사용하지 않는다.
- authoritative state는 world policy key + continuity world owner key 조합이다.
- `GET/PUT/DELETE /api/v1/worlds/{world_id}/drain` 도 fanout/admin 명령 publish를 사용하지 않는다.
- authoritative state는 기존 Redis `dynaxis:continuity:world-policy:<world_id>` key이고, named drain surface는 그 위에 live progress/status를 붙인다.
- `GET/PUT/DELETE /api/v1/worlds/{world_id}/migration` 도 fanout/admin 명령 publish를 사용하지 않는다.
- authoritative state는 `dynaxis:continuity:world-migration:<world_id>` envelope key다.
- 따라서 이 엔드포인트들은 `ADMIN_COMMAND_SIGNING_SECRET` 없이도 동작한다.

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
- `GET /api/v1/worlds/{world_id}/drain`
  - 현재 world inventory 위에서 평가한 world-drain phase/progress와 orchestration handoff 상태를 반환한다
  - `orchestration.phase`는 `idle|blocked_by_replacement_target|draining|awaiting_owner_transfer|awaiting_migration|ready_to_clear`
  - `orchestration.next_action`은 `none|wait_for_drain|stabilize_replacement_target|commit_owner_transfer|await_migration|clear_policy`
- `PUT /api/v1/worlds/{world_id}/drain`
  - JSON body required
  - `replacement_owner_instance_id` (선택, string 또는 `null`)
  - optional replacement target을 기록하면서 named drain을 선언한다
- `DELETE /api/v1/worlds/{world_id}/drain`
  - named drain policy를 해제한다
  - 현재 지원하는 closure flow는 `drain -> owner transfer commit 또는 migration readiness 확인 -> ready_to_clear -> DELETE /drain` 형태다
- `GET /api/v1/worlds/{world_id}/transfer`
  - 현재 world inventory 위에서 평가한 owner-transfer 상태를 반환한다
- `PUT /api/v1/worlds/{world_id}/transfer`
  - JSON body required
  - `target_owner_instance_id` (필수, string)
  - `expected_owner_instance_id` (선택, string 또는 `null`)
  - `commit_owner` (선택, bool, 기본 `false`)
  - same-world `server` replacement target만 허용한다
  - `commit_owner=true`면 continuity world owner key를 replacement owner로 즉시 커밋한다
- `DELETE /api/v1/worlds/{world_id}/transfer`
  - transfer lifecycle policy(`draining + replacement_owner_instance_id`)만 해제한다
  - 이미 커밋된 owner boundary는 유지한다
- `GET /api/v1/worlds/{world_id}/migration`
  - source world 기준 migration envelope/status를 반환한다
- `PUT /api/v1/worlds/{world_id}/migration`
  - JSON body required
  - `target_world_id` (필수, source와 달라야 함)
  - `target_owner_instance_id` (필수, target world의 known server instance)
  - `preserve_room` (선택, bool)
  - `payload_kind` / `payload_ref` (선택, opaque app-defined handoff metadata)
  - 현재 `server_app` 해석 규칙: `payload_kind="chat-room-v1"`이면 `payload_ref`를 resumed target room으로 해석한다
- `DELETE /api/v1/worlds/{world_id}/migration`
  - 저장된 migration envelope를 제거한다

설정 key/range 검증은 `server/include/server/config/runtime_settings.hpp`의 공통 규칙을 사용한다.
동일 규칙이 `admin_app` 입력 검증과 `server_app` 런타임 적용 경로에 함께 적용되어 문서와 코드의 어긋남(drift)을 줄인다.

권한:

- `viewer`: 조회 전용(read-only)
- `operator`: 공지(announcement) 가능
- `admin`: runtime settings + moderation + world lifecycle policy + world drain + world owner transfer + world migration 포함 전체 가능

`/api/v1/auth/context` capability에는 `world_drain`, `world_transfer`, `world_migration`, `desired_topology`, `topology_actuation_request`, `topology_actuation_execution`, `topology_actuation_adapter`, `topology_actuation_runtime_assignment`가 추가되며, 현재는 여덟 capability 모두 `admin`만 write 가능하다.

## 원하는 토폴로지(desired topology) 제어

- `GET /api/v1/topology/desired`
- `PUT /api/v1/topology/desired`
- `DELETE /api/v1/topology/desired`

`desired topology`는 world lifecycle policy와 별도다.

이 분리가 중요한 이유는, lifecycle policy는 "지금 이미 존재하는 world를 어떻게 전환할 것인가"에 가깝고, desired topology는 "전체적으로 어떤 pool 배치를 원하는가"에 가깝기 때문이다. 둘을 섞으면 배치 의도와 개별 world 조작이 한 문서에서 충돌할 수 있다.

- revision이 붙는 문서 형태:
  - `topology_id`
  - `revision`
  - `updated_at_ms`
  - `pools[]`
- 각 pool 항목:
  - `world_id`
  - `shard`
  - `replicas`
  - `capacity_class` (선택)
  - `placement_tags[]` (선택)
- `PUT` 요청 본문:
  - `topology_id` (필수)
  - `pools[]` (필수)
  - `expected_revision` (선택, 낙관적 revision 검사)
- `expected_revision`이 현재 저장된 revision과 다르면 `409 REVISION_MISMATCH`를 반환한다.
- `DELETE`는 저장된 desired topology 문서를 제거한다.

## 관측 토폴로지(observed topology) 조회 모델

- `GET /api/v1/topology/observed`

이 엔드포인트는 `desired topology`와 별도인 현재 runtime 조회 모델이다.

의도 문서와 관측 문서를 분리하는 이유는, 운영자가 "우리가 원한 상태"와 "실제로 떠 있는 상태"를 같은 JSON 안에서 혼동하지 않게 하기 위해서다. 실제 장애에서는 두 문서가 다를 가능성이 높고, 바로 그 차이가 문제의 핵심이다.

- `instances[]`
  - `instance_id`, `role`, `host`, `port`, `game_mode`, `region`, `shard`, `ready`
  - `world_scope` (server instance일 때 world owner/policy 가시성)
- `worlds[]`
  - 현재 `GET /api/v1/worlds` inventory와 동일한 world owner/policy summary
- `summary.instances_total`
- `summary.worlds_total`
- `updated_at_ms`

## 토폴로지 재조정(reconciliation) 뷰

- `GET /api/v1/topology/reconciliation`

이 엔드포인트는 저장된 desired topology 문서와 현재 observed pool을 비교한 재조정 뷰다.

reconciliation을 별도 엔드포인트로 둔 이유는, 단순 observed 목록만 봐서는 "문제가 있는지"를 사람이 매번 계산해야 하기 때문이다. 이 뷰는 replica 부족, replica 과다, 선언되지 않은 pool 같은 상태를 명시적으로 계산해 운영 판단 시간을 줄인다.

- `desired`
  - 저장된 desired topology 문서 또는 `null`
- `observed_pools[]`
  - `world_id`, `shard`, `instances`, `ready_instances`
- `summary`
  - `desired_present`
  - `desired_pools`
  - `observed_pools`
  - `aligned_pools`
  - `missing_pools`
  - `under_replicated_pools`
  - `over_replicated_pools`
  - `undeclared_pools`
  - `no_ready_pools`
- `pools[]`
  - `world_id`, `shard`
  - `desired_replicas`
  - `observed_instances`
  - `ready_instances`
  - `status`

## 토폴로지 실행 계획(actuation) 뷰

- `GET /api/v1/topology/actuation`

이 엔드포인트는 저장된 desired topology와 observed pool을 바탕으로, 다음에 필요한 runtime/operator action을 읽기 전용으로 계산한 실행 계획 뷰다.

이 단계가 따로 필요한 이유는, "무엇을 해야 하는가"와 "정말 그걸 승인했는가"를 분리하기 위해서다. 자동 계산 결과를 곧바로 쓰기 명령으로 취급하면, 잘못된 관측이나 일시적 흔들림이 즉시 운영 변경으로 이어질 수 있다.

- `desired`
  - 저장된 desired topology 문서 또는 `null`
- `observed_pools[]`
  - `world_id`, `shard`, `instances`, `ready_instances`
- `summary`
  - `desired_present`
  - `actions_total`
  - `actionable_actions`
  - `scale_out_actions`
  - `scale_in_actions`
  - `readiness_recovery_actions`
  - `observe_only_actions`
- `actions[]`
  - `world_id`, `shard`
  - `status` (`missing_observed_pool|under_replicated|over_replicated|no_ready_instances|undeclared_observed_pool`)
  - `action` (`scale_out_pool|scale_in_pool|restore_pool_readiness|observe_undeclared_pool`)
  - `actionable`
  - `desired_replicas`
  - `observed_instances`
  - `ready_instances`
  - `replica_delta`

## 토폴로지 실행 요청

- `GET /api/v1/topology/actuation/request`
- `PUT /api/v1/topology/actuation/request`
- `DELETE /api/v1/topology/actuation/request`

이 엔드포인트는 읽기 전용 계획과 분리된, 운영자 승인 실행 요청 문서를 저장한다.

즉 이 문서는 "계산 결과"가 아니라 "운영자가 실제로 하겠다고 승인한 일"이다. 이 승인 경계를 분리해 두면 나중에 누가 어떤 기준 revision 위에서 scale-out 또는 scale-in을 요청했는지 감사 추적이 쉬워진다.

- 응답 문서:
  - `key`
  - `present`
  - `request`
- `request`
  - `request_id`
  - `revision`
  - `requested_at_ms`
  - `basis_topology_revision`
  - `actions[]`
- 각 action:
  - `world_id`
  - `shard`
  - `action`
  - `replica_delta`
- `PUT` 요청 본문:
  - `request_id` (필수)
  - `basis_topology_revision` (필수)
  - `actions[]` (필수, non-empty)
  - `expected_revision` (선택)
- `PUT`은 현재 `GET /api/v1/topology/actuation`에서 실제 실행 가능(actionable)로 보이는 action만 허용한다.
- `observe_undeclared_pool`는 observe-only 범주이므로 request로 저장할 수 없다.

## 토폴로지 실행 상태

- `GET /api/v1/topology/actuation/status`

이 엔드포인트는 현재 저장된 실행 요청을 현재 읽기 전용 실행 계획과 비교해, 요청이 아직 유효한지 계산한 상태 뷰다.

이 뷰가 없으면 운영자는 예전에 승인했던 요청이 지금도 맞는지 매번 수동으로 비교해야 한다. status는 pending, satisfied, superseded를 계산해 "아직 해야 하는가, 이미 충족됐는가, 상황이 바뀌었는가"를 한눈에 보여 준다.

- `desired`
  - 현재 desired topology 문서 또는 `null`
- `request`
  - 현재 저장된 actuation request 문서 또는 `null`
- `observed_pools[]`
  - `world_id`, `shard`, `instances`, `ready_instances`
- `plan_summary`
  - 현재 읽기 전용 actuation plan 요약
- `summary`
  - `request_present`
  - `desired_present`
  - `basis_topology_revision_matches_current`
  - `basis_topology_revision`
  - `current_topology_revision`
  - `actions_total`
  - `pending_actions`
  - `satisfied_actions`
  - `superseded_actions`
- `actions[]`
  - `world_id`, `shard`
  - `requested_action`
  - `requested_replica_delta`
  - `state` (`pending|satisfied|superseded`)
  - `current_status`
  - `current_action`
  - `current_replica_delta`

## 토폴로지 실행 진행도

- `GET /api/v1/topology/actuation/execution`
- `PUT /api/v1/topology/actuation/execution`
- `DELETE /api/v1/topology/actuation/execution`

이 엔드포인트는 현재 실행 요청에 대해 실행기(executor)가 어디까지 진행했는지 기록하는 진행도 문서를 저장한다.

request와 execution을 분리하는 이유는 명확하다. request는 "무엇을 하라고 승인됐는가"이고, execution은 "실행기가 실제로 무엇을 잡아서 처리 중인가"다. 둘을 섞으면 claim, completed, failed 같은 실행 중 상태를 표현하기 어렵다.

- 응답 문서:
  - `key`
  - `present`
  - `execution`
- `execution`
  - `executor_id`
  - `revision`
  - `updated_at_ms`
  - `request_revision`
  - `actions[]`
- 각 action:
  - `world_id`
  - `shard`
  - `action`
  - `replica_delta`
  - `observed_instances_before`
  - `ready_instances_before`
  - `state` (`claimed|completed|failed`)
- `PUT` 요청 본문:
  - `executor_id` (필수)
  - `request_revision` (필수)
  - `actions[]` (필수, non-empty)
  - `expected_revision` (선택)
- `PUT`은 현재 actuation request revision과 정확히 맞는 항목만 허용한다.
- 첫 claim에서는 기준선(`observed_instances_before`, `ready_instances_before`)이 현재 observed pool과 일치해야 한다.
- 같은 pool을 다시 업데이트할 때는 기준선이 기존 execution 문서와 동일해야 한다.

## 토폴로지 실행 진행 상태

- `GET /api/v1/topology/actuation/execution/status`

이 엔드포인트는 현재 실행 요청과 현재 실행 진행도 문서를 합쳐, 실행기 입장에서 지금 어떤 action이 available, claimed, completed, failed, stale인지 계산한 상태 뷰다.

이 계층이 별도인 이유는, 원시 execution 문서만 봐서는 아직 잡을 수 있는 작업과 이미 오래되어 무효가 된 작업을 구분하기 어렵기 때문이다.

- `request`
  - 현재 저장된 actuation request 문서 또는 `null`
- `execution`
  - 현재 저장된 execution progress 문서 또는 `null`
- `request_summary`
  - 현재 actuation request 상태 요약
- `summary`
  - `request_present`
  - `execution_present`
  - `execution_revision_matches_current_request`
  - `execution_request_revision`
  - `current_request_revision`
  - `actions_total`
  - `available_actions`
  - `claimed_actions`
  - `completed_actions`
  - `failed_actions`
  - `stale_actions`
- `actions[]`
  - `world_id`, `shard`
  - `requested_action`
  - `requested_replica_delta`
  - `state` (`available|claimed|completed|failed|stale`)
  - `request_state`
  - `execution_state`

## 토폴로지 반영 상태

- `GET /api/v1/topology/actuation/realization`

이 엔드포인트는 현재 request/execution 문서와 observed pool을 합쳐, 실행기의 `completed`가 실제 topology 반영(realization)으로 이어졌는지 계산한 뷰다.

이 단계가 중요한 이유는 "실행기가 완료라고 표시했다"와 "클러스터/런타임이 실제로 그렇게 변했다"가 같은 뜻이 아니기 때문이다. realization은 그 둘을 분리해 거짓 완료(false completion)를 줄인다.

- `request`
  - 현재 저장된 actuation request 문서 또는 `null`
- `execution`
  - 현재 저장된 execution progress 문서 또는 `null`
- `observed_pools[]`
  - `world_id`, `shard`, `instances`, `ready_instances`
- `request_summary`
  - 현재 actuation request 상태 요약
- `execution_summary`
  - 현재 actuation execution 상태 요약
- `summary`
  - `request_present`
  - `execution_present`
  - `execution_revision_matches_current_request`
  - `execution_request_revision`
  - `current_request_revision`
  - `actions_total`
  - `available_actions`
  - `claimed_actions`
  - `awaiting_observation_actions`
  - `realized_actions`
  - `failed_actions`
  - `stale_actions`
- `actions[]`
  - `world_id`, `shard`
  - `requested_action`
  - `requested_replica_delta`
  - `state` (`available|claimed|awaiting_observation|realized|failed|stale`)
  - `request_state`
  - `execution_state`
  - `observed_instances_before`
  - `ready_instances_before`
  - `current_observed_instances`
  - `current_ready_instances`

## 토폴로지 실행 어댑터

- `GET /api/v1/topology/actuation/adapter`
- `PUT /api/v1/topology/actuation/adapter`
- `DELETE /api/v1/topology/actuation/adapter`

이 엔드포인트는 현재 actuation execution 문서 위에, 실제 확장 시스템에 연결되는 scaling adapter가 잡은 lease 문서를 저장한다.

adapter lease를 분리하는 이유는 실행기와 실제 배포 시스템 어댑터를 같은 책임으로 보지 않기 위해서다. 실행 계획을 처리하는 주체와 Kubernetes, AWS, 기타 실행기를 붙이는 주체는 나중에 달라질 수 있으므로 lease 경계를 둔다.

- 응답 문서:
  - `key`
  - `present`
  - `lease`
- `lease`
  - `adapter_id`
  - `revision`
  - `leased_at_ms`
  - `execution_revision`
  - `actions[]`
- 각 action:
  - `world_id`
  - `shard`
  - `action`
  - `replica_delta`
- `PUT` 요청 본문:
  - `adapter_id` (필수)
  - `execution_revision` (필수)
  - `actions[]` (필수, non-empty)
  - `expected_revision` (선택)
- `PUT`은 현재 execution revision과 정확히 일치하는 action만 허용한다.
- `PUT`은 현재 adapter 상태 기준으로 `stale`인 action을 lease로 저장할 수 없다.

## 토폴로지 실행 어댑터 상태

- `GET /api/v1/topology/actuation/adapter/status`

이 엔드포인트는 현재 execution progress, realization evidence, 저장된 adapter lease를 합쳐 adapter 관점의 유효 상태를 계산한 뷰다.

이 뷰는 adapter가 "아직 내가 잡아야 하는가, 이미 반영됐는가, stale해서 버려야 하는가"를 계산하는 데 필요하다. 원시 lease 문서만으로는 이런 판단이 충분하지 않다.

- `lease`
  - 현재 저장된 adapter lease 문서 또는 `null`
- `execution`
  - 현재 저장된 execution progress 문서 또는 `null`
- `execution_summary`
  - 현재 actuation execution 상태 요약
- `realization_summary`
  - 현재 actuation realization 상태 요약
- `summary`
  - `execution_present`
  - `lease_present`
  - `lease_revision_matches_current_execution`
  - `lease_execution_revision`
  - `current_execution_revision`
  - `actions_total`
  - `available_actions`
  - `leased_actions`
  - `awaiting_realization_actions`
  - `realized_actions`
  - `failed_actions`
  - `stale_actions`
- `actions[]`
  - `world_id`, `shard`
  - `requested_action`
  - `requested_replica_delta`
  - `state` (`available|leased|awaiting_realization|realized|failed|stale`)
  - `execution_state`
  - `realization_state`

## 토폴로지 런타임 배치

- `GET /api/v1/topology/actuation/runtime-assignment`
- `PUT /api/v1/topology/actuation/runtime-assignment`
- `DELETE /api/v1/topology/actuation/runtime-assignment`

이 엔드포인트는 현재 adapter lease를 소비해, 실제로 실행 중인 `server` instance 재배치로 연결하는 앱 로컬(app-local) 런타임 변경 표면이다.

이 계층을 따로 둔 이유는 토폴로지 문서와 실제 런타임 변경(runtime mutation)의 위험도가 다르기 때문이다. 문서 수준에서는 계획과 승인만 표현하고, 진짜 인스턴스 재배치는 더 좁은 조건과 검증 아래에서만 수행하는 편이 안전하다.

- 응답 문서:
  - `key`
  - `present`
  - `assignment`
- `assignment`
  - `adapter_id`
  - `revision`
  - `updated_at_ms`
  - `lease_revision`
  - `assignments[]`
- 각 assignment:
  - `instance_id`
  - `world_id`
  - `shard`
  - `action`
- `PUT` 요청 본문:
  - `adapter_id` (필수)
  - `lease_revision` (필수)
  - `assignments[]` (필수, non-empty)
  - `expected_revision` (선택)
- 현재 구현은 의도적으로 좁다:
  - `action=scale_out_pool`만 허용한다
  - 대상 instance는 현재 observed topology에 존재하는 `role=server`, `ready=true`, `active_sessions=0`이어야 한다
  - assignment는 현재 adapter lease와 일치해야 하며 lease replica capacity를 초과할 수 없다
  - assignment가 적용되면 matching `SERVER_INSTANCE_ID`는 registry heartbeat와 fresh world admission default에서 new `world_id` / `shard`를 사용한다

## 확장 배포 제어면

`/api/v1/ext/*`는 플러그인/스크립트 배포 제어를 위한 API다.

이 영역은 토폴로지와 별개로 "실행 중 기능을 어떻게 안전하게 배포할 것인가"를 다룬다. precheck, deployment, schedule을 분리한 이유도 같아서, 충돌 검증과 실제 배포, 예약 실행을 한 단계로 섞지 않으려는 것이다.

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
  - 스케줄러가 UTC 기준으로 실행
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

## 개요 트렌드

`GET /api/v1/overview`는 `counts` 외에 `audit_trend`를 제공한다.

- `counts.http_server_errors_total`
- `audit_trend.step_ms`, `audit_trend.max_points`
- `audit_trend.points[]` (`timestamp_ms`, `http_errors_total`, `http_server_errors_total`, `http_unauthorized_total`, `http_forbidden_total`)
