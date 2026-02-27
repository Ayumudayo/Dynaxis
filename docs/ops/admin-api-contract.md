# 관리자 API 계약 (2단계: 제어면)

본 문서는 `admin_app` API 계약(2단계)을 정의한다.

- 목표: 운영자가 분산 상태 조회 + 제한적 운영 액션을 한 곳에서 수행
- 범위: 조회(read) + 제한적 쓰기(write)(연결 종료/공지/런타임 설정)
- 비범위: drain/undrain, 권한 정책 변경, 영구 설정 저장

관련 문서:

- 아키텍처/범위: `docs/ops/admin-console.md`
- 운영 관측: `docs/ops/observability.md`

## 1. 버전/경로

- 기본 경로: `/api/v1`
- `Content-Type`: `application/json; charset=utf-8`
- 응답은 UTF-8 JSON

## 2. 인증/인가 계약

기본 정책:

1. 내부 네트워크 접근만 허용 (VPN/allowlist)
2. `admin_app` 앞단 reverse proxy에서 인증 수행
3. `admin_app`은 신뢰된 identity header 또는 bearer 토큰을 해석

2단계 구현 명세(현재):

- 보호 경로: `/admin`, `/api/v1/*`
- 공개 경로: `/healthz`, `/readyz`, `/metrics`
- `ADMIN_AUTH_MODE`:
  - `off` (기본값): 인증 비활성(내부망 전용 운영 전제, role=`ADMIN_OFF_ROLE`)
  - `header`: identity header만 허용
  - `bearer`: bearer 토큰만 허용
  - `header_or_bearer`: header 우선, 미존재 시 bearer 허용
- Header 모드 기본 header 이름:
  - user: `X-Admin-User` (`ADMIN_AUTH_USER_HEADER`로 변경 가능)
  - role: `X-Admin-Role` (`ADMIN_AUTH_ROLE_HEADER`로 변경 가능)
- Header 모드 role 규칙:
  - role 미지정 시 `viewer`로 간주
  - `viewer|operator|admin` 외 값은 `FORBIDDEN(403)`
- Bearer 모드 스펙:
  - `Authorization: Bearer <token>`
  - 토큰 비교값: `ADMIN_BEARER_TOKEN`
  - actor/role 주입값: `ADMIN_BEARER_ACTOR`, `ADMIN_BEARER_ROLE`
  - token 미설정/불일치/형식오류는 `UNAUTHORIZED(401)`
- Off 모드 스펙:
  - actor=`anonymous`
  - role=`ADMIN_OFF_ROLE` (기본 `admin`)

최소 역할(role):

- `viewer`: 조회 API만 접근 가능
- `operator`: 조회 + 연결 종료/공지
- `admin`: 조회 + 연결 종료/공지 + 런타임 설정 변경

## 3. 공통 응답 형식

성공:

```json
{
  "data": {},
  "meta": {
    "request_id": "3af9d3df-9b4f-4fc2-a92c-c36fd9471ccf",
    "generated_at_ms": 1760535000123
  }
}
```

실패:

```json
{
  "error": {
    "code": "NOT_FOUND",
    "message": "session mapping not found",
    "details": {
      "client_id": "user-123"
    }
  },
  "meta": {
    "request_id": "3af9d3df-9b4f-4fc2-a92c-c36fd9471ccf",
    "generated_at_ms": 1760535000123
  }
}
```

## 4. 공통 에러 코드

- `UNAUTHORIZED` (401)
- `FORBIDDEN` (403)
- `NOT_FOUND` (404)
- `BAD_REQUEST` (400)
- `UPSTREAM_UNAVAILABLE` (503)
- `TIMEOUT` (504)
- `INTERNAL_ERROR` (500)

## 5. 공통 쿼리 파라미터

- `timeout_ms` (선택, 기본 1500, 최대 5000)
- `limit` (선택, 기본 100, 최대 500)
- `cursor` (선택)

## 6. 엔드포인트 계약

### 6.0 GET /api/v1/auth/context

목적:

- 현재 요청의 인증 컨텍스트(actor/role/mode) 확인

응답 필드:

- `actor`
- `role`
- `mode`
- `headers.user`, `headers.role`

메모:

- UI는 해당 엔드포인트를 사용해 현재 로그인 컨텍스트를 표시한다.
- `mode=off`면 내부망 접근 정책 하에서 익명 + `ADMIN_OFF_ROLE` 컨텍스트로 응답한다.

### 6.1 GET /api/v1/overview

목적:

- 운영 메인 대시보드용 요약

응답 필드:

- `services`: `gateway`, `server`, `wb_worker`, `haproxy` 요약
- `counts`: ready/up/down 집계 + `http_errors_total`, `http_server_errors_total`, `http_unauthorized_total`, `http_forbidden_total`
- `audit_trend`: 최근 카운터 스냅샷 (`step_ms`, `max_points`, `points[]`)
- `generated_at_ms`

`audit_trend.points[]` 필드:

- `timestamp_ms`
- `http_errors_total`
- `http_server_errors_total`
- `http_unauthorized_total`
- `http_forbidden_total`

데이터 소스:

- Prometheus scrape/queries
- 각 서비스 admin 엔드포인트 (`/healthz`, `/readyz`, `/metrics`)

### 6.2 GET /api/v1/instances

목적:

- backend 인스턴스 상태 조회

응답 필드(항목별):

- `instance_id`
- `host`, `port`
- `role`
- `ready`
- `active_sessions`
- `last_heartbeat_ms`
- `source.registry_key`

데이터 소스:

- Redis key pattern: `{SERVER_REGISTRY_PREFIX}{instance_id}`
- 기본 prefix: `gateway/instances/`

### 6.3 GET /api/v1/instances/{instance_id}

목적:

- 단일 인스턴스 상세 조회

응답 필드:

- `/api/v1/instances` 항목 + `metrics_url`, `ready_reason`

`ready_reason`은 `/readyz` 응답 본문(body)을 그대로 포함한다.

### 6.4 GET /api/v1/sessions/{client_id}

목적:

- sticky session 매핑 확인

응답 필드:

- `client_id`
- `backend_instance_id` (없으면 null)
- `backend` (instance 상세 요약)
- `source.session_key`

데이터 소스:

- Redis key: `gateway/session/{client_id}`

메모:

- `client_id`는 gateway 인증 결과 subject를 기준으로 사용한다.

### 6.5 GET /api/v1/worker/write-behind

목적:

- write-behind 상태 요약

응답 필드:

- `pending`
- `flush_total`, `flush_ok_total`, `flush_fail_total`, `flush_dlq_total`
- `ack_total`, `ack_fail_total`
- `reclaim_total`, `reclaim_error_total`

데이터 소스:

- `wb_worker` metrics (`/metrics`)

### 6.6 GET /api/v1/metrics/links

목적:

- Grafana/Prometheus deep-link 제공

응답 필드:

- `grafana.base_url`
- `prometheus.base_url`
- `dashboards[]`
- `queries[]`

### 6.7 GET /api/v1/users

목적:

- 현재 접속 사용자 목록 조회(운영 선택/조치 대상)

쿼리:

- 공통: `limit`, `cursor`
- 추가: `room` (선택)

응답 필드(항목별):

- `client_id`
- `room`
- `backend_instance_id` (없으면 null)
- `backend` (instance 요약, 없으면 null)
- `source.session_key`
- `source.room_key`

데이터 소스:

- Redis: `room:users:*`, `gateway/session/{client_id}`
- instance 캐시: `/api/v1/instances` 내부 캐시 재사용

### 6.8 POST /api/v1/users/disconnect

목적:

- 선택한 사용자 세션 강제 종료 명령 발행

쿼리:

- `client_id` 또는 `client_ids`(쉼표 구분) 중 하나 이상 필수
- `reason` (선택)

동작:

- Redis Pub/Sub channel `${REDIS_CHANNEL_PREFIX}fanout:admin:disconnect`로 최선 시도(best-effort) 발행
- payload는 `client_ids`, `reason`, `actor`, `request_id`를 포함

권한:

- `admin`

### 6.9 POST /api/v1/announcements

목적:

- 서버 전체 공지 명령 발행

쿼리:

- `text` (필수, 최대 512 bytes)
- `priority` (`info|warn|critical`, 선택)

동작:

- Redis Pub/Sub channel `${REDIS_CHANNEL_PREFIX}fanout:admin:announce`

권한:

- `operator`, `admin`

### 6.10 PATCH /api/v1/settings

목적:

- 런타임 변경 가능한 설정 변경 명령 발행

쿼리:

- `key` (필수)
  - `presence_ttl_sec` (`5..3600`)
  - `recent_history_limit` (`5..2000`)
  - `room_recent_maxlen` (`5..5000`)
- `value` (필수, 부호 없는 정수)

구현 메모:

- key/range 규칙은 `server/include/server/config/runtime_settings.hpp`를 단일 기준 원천으로 사용한다.
- `admin_app` 입력 검증과 `server_app` 적용 검증이 동일 규칙을 공유한다.

동작:

- Redis Pub/Sub channel `${REDIS_CHANNEL_PREFIX}fanout:admin:settings`

권한:

- `admin`

## 7. 권한 매트릭스

| 엔드포인트 | viewer | operator | admin |
| --- | --- | --- | --- |
| GET /api/v1/auth/context | 허용 | 허용 | 허용 |
| GET /api/v1/overview | 허용 | 허용 | 허용 |
| GET /api/v1/instances | 허용 | 허용 | 허용 |
| GET /api/v1/instances/{instance_id} | 허용 | 허용 | 허용 |
| GET /api/v1/users | 허용 | 허용 | 허용 |
| GET /api/v1/sessions/{client_id} | 허용 | 허용 | 허용 |
| POST /api/v1/users/disconnect | 거부 | 거부 | 허용 |
| POST /api/v1/announcements | 거부 | 허용 | 허용 |
| PATCH /api/v1/settings | 거부 | 거부 | 허용 |
| GET /api/v1/worker/write-behind | 허용 | 허용 | 허용 |
| GET /api/v1/metrics/links | 허용 | 허용 | 허용 |

## 8. 감사 로그 계약

모든 요청에 대해 아래 필드 기록:

- `request_id`
- `actor`
- `role`
- `method`
- `path`
- `resource`
- `result` (`ok` or `error`)
- `status_code`
- `latency_ms`
- `source_ip`
- `timestamp`

민감정보 원칙:

- 토큰/비밀번호/원본 Authorization 헤더는 기록 금지
- Redis/Postgres credential 기록 금지

## 9. 성능/안전 가드

- 모든 upstream 호출 타임아웃(timeout) 기본 1500ms
- fan-out 조회에는 제한 동시성(bounded concurrency)을 적용
- Redis key 조회는 prefix 고정 + 과도한 scan 회피
- 엔드포인트별 응답 크기 제한 (예: 1MB)

## 10. 구현 메모 (현재 코드베이스 연동)

- health/ready/metrics 기반:
  - `core/src/metrics/http_server.cpp`
  - `core/src/app/app_host.cpp`
- server `/logs` 제공:
  - `server/src/app/metrics_server.cpp`
- instance registry:
  - `server/src/state/instance_registry.cpp`
- session directory:
  - `gateway/src/session_directory.cpp`
  - `gateway/src/gateway_app.cpp`
- fanout whisper:
  - `server/src/app/bootstrap.cpp`
  - `server/src/chat/chat_service_core.cpp`

## 11. 오픈 이슈

1. `client_id` 표준(canonical) 규칙 확정 (사용자명(username) vs uuid subject)
2. multi-gateway 환경에서 fanout 관측 요약 지표 추가 여부
