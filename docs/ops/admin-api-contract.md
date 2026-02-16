# Admin API Contract (Phase 1: Read-only)

본 문서는 `admin_app`의 1차 API 계약을 정의한다.

- 목표: 운영자가 분산 상태를 한 곳에서 조회
- 범위: 조회(Read-only)만 허용
- 비범위: 세션 강제 종료, drain/undrain, 설정 변경 같은 write 액션

관련 문서:

- 아키텍처/범위: `docs/ops/admin-console.md`
- 운영 관측: `docs/ops/observability.md`

## 1. 버전/경로

- Base path: `/api/v1`
- Content-Type: `application/json; charset=utf-8`
- 응답은 UTF-8 JSON

## 2. 인증/인가 계약

Phase 1 기본 정책:

1. 내부 네트워크 접근만 허용 (VPN/allowlist)
2. `admin_app` 앞단 reverse proxy에서 인증 수행
3. `admin_app`은 신뢰된 identity header 또는 bearer 토큰을 해석

Phase 1 구현 스펙(현재):

- 보호 경로: `/admin`, `/api/v1/*`
- 공개 경로: `/healthz`, `/readyz`, `/metrics`
- `ADMIN_AUTH_MODE`:
  - `off` (기본값): 인증 비활성(내부망 전용 운영 전제)
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

최소 role:

- `viewer`: 모든 Phase 1 조회 API 접근 가능
- `operator`, `admin`: Phase 1에서는 `viewer`와 동일 권한

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

- `timeout_ms` (optional, default 1500, max 5000)
- `limit` (optional, default 100, max 500)
- `cursor` (optional)

## 6. Endpoint 계약

### 6.0 GET /api/v1/auth/context

목적:

- 현재 요청의 인증 컨텍스트(actor/role/mode) 확인

응답 필드:

- `actor`
- `role`
- `mode`
- `headers.user`, `headers.role`

메모:

- UI는 해당 endpoint를 사용해 현재 로그인 컨텍스트를 표시한다.
- `mode=off`면 내부망 접근 정책 하에서 익명 `viewer` 컨텍스트로 응답한다.

### 6.1 GET /api/v1/overview

목적:

- 운영 메인 대시보드용 요약

응답 필드:

- `services`: `gateway`, `server`, `wb_worker`, `haproxy` 요약
- `counts`: ready/up/down 집계 + `http_errors_total`, `http_unauthorized_total`, `http_forbidden_total`
- `generated_at_ms`

데이터 소스:

- Prometheus scrape/queries
- 각 서비스 admin endpoint (`/healthz`, `/readyz`, `/metrics`)

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

`ready_reason`은 `/readyz` body를 그대로 포함한다.

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

## 7. 권한 매트릭스

| Endpoint | viewer | operator | admin |
| --- | --- | --- | --- |
| GET /api/v1/auth/context | allow | allow | allow |
| GET /api/v1/overview | allow | allow | allow |
| GET /api/v1/instances | allow | allow | allow |
| GET /api/v1/instances/{instance_id} | allow | allow | allow |
| GET /api/v1/sessions/{client_id} | allow | allow | allow |
| GET /api/v1/worker/write-behind | allow | allow | allow |
| GET /api/v1/metrics/links | allow | allow | allow |

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

- 모든 upstream 호출 timeout 기본 1500ms
- fan-out 조회는 bounded concurrency 적용
- Redis key 조회는 prefix 고정 + 과도한 scan 회피
- endpoint별 응답 크기 제한 (예: 1MB)

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

1. `client_id` canonical 규칙 확정 (username vs uuid subject)
2. multi-gateway 환경에서 fanout 관측 요약 지표 추가 여부
