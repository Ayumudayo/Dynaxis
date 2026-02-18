# admin_app

`admin_app`은 운영 관리 콘솔을 위한 control-plane 서비스다.

현재 제공 endpoint:

- `/metrics`
- `/healthz`, `/readyz`
- `/admin` (browser UI, source: `tools/admin_app/admin_ui.html`)
- `/api/v1/auth/context` (JSON)
- `/api/v1/overview` (JSON)
- `/api/v1/instances` (JSON)
- `/api/v1/instances/{instance_id}` (JSON)
- `/api/v1/sessions/{client_id}` (JSON)
- `/api/v1/users` (JSON)
- `/api/v1/users/disconnect` (POST, query)
- `/api/v1/announcements` (POST, query)
- `/api/v1/settings` (PATCH, query)
- `/api/v1/worker/write-behind` (JSON)
- `/api/v1/metrics/links` (JSON)

## 환경 변수

| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `METRICS_PORT` | admin HTTP 포트 | `39200` |
| `ADMIN_POLL_INTERVAL_MS` | 백그라운드 상태 수집 주기(ms) | `1000` |
| `ADMIN_AUDIT_TREND_MAX_POINTS` | overview audit_trend 히스토리 최대 포인트 수 | `300` |
| `ADMIN_INSTANCE_METRICS_PORT` | instance detail 조회 시 metrics/ready probe 포트 | `9090` |
| `REDIS_URI` | Redis 연결 문자열(인스턴스/세션 조회용) | (unset) |
| `SERVER_REGISTRY_PREFIX` | 인스턴스 레지스트리 prefix | `gateway/instances/` |
| `GATEWAY_SESSION_PREFIX` | 세션 디렉터리 key prefix | `gateway/session/` |
| `REDIS_CHANNEL_PREFIX` | fanout/admin command channel prefix | `` |
| `WB_WORKER_METRICS_URL` | wb_worker metrics URL | `http://127.0.0.1:39093/metrics` |
| `GRAFANA_BASE_URL` | Grafana base URL 링크 | `http://127.0.0.1:33000` |
| `PROMETHEUS_BASE_URL` | Prometheus base URL 링크 | `http://127.0.0.1:39090` |
| `ADMIN_AUTH_MODE` | 인증 모드 (`off`, `header`, `bearer`, `header_or_bearer`) | `off` |
| `ADMIN_OFF_ROLE` | `ADMIN_AUTH_MODE=off`일 때 적용할 role (`viewer/operator/admin`) | `admin` |
| `ADMIN_AUTH_USER_HEADER` | header 인증 시 사용자 header 이름 | `X-Admin-User` |
| `ADMIN_AUTH_ROLE_HEADER` | header 인증 시 역할 header 이름 | `X-Admin-Role` |
| `ADMIN_BEARER_TOKEN` | bearer 인증 토큰 값 | (unset) |
| `ADMIN_BEARER_ACTOR` | bearer 인증 성공 시 actor 값 | `token-user` |
| `ADMIN_BEARER_ROLE` | bearer 인증 성공 시 role 값 (`viewer/operator/admin`) | `viewer` |

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

## Docker Stack

`docker/stack`에 `admin-app` 서비스가 포함된다.

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability
```

브라우저 접속:

- `http://127.0.0.1:39200/admin`

## 참고 문서

- `docs/ops/admin-console.md`
- `docs/ops/admin-api-contract.md`
- `docs/ops/admin-gui-wireframe.md`

## 공통 쿼리 파라미터

`/api/v1/*`는 다음 쿼리 파라미터를 지원한다.

- `timeout_ms` (optional, max `5000`)
- `limit` (optional, max `500`)
- `cursor` (optional)

## Write Actions (Phase 2)

`admin_app`는 아래 write endpoint를 제공한다. 현재 MetricsHttpServer 제약으로 body 대신 query parameter를 사용한다.

- `POST /api/v1/users/disconnect`
  - `client_id` 또는 `client_ids`(comma separated)
  - `reason` (optional)
- `POST /api/v1/announcements`
  - `text` (required, max 512 bytes)
  - `priority` (`info|warn|critical`, optional)
- `PATCH /api/v1/settings`
  - `key` (`presence_ttl_sec|recent_history_limit|room_recent_maxlen`)
  - `value` (unsigned integer)

권한:

- `viewer`: read-only
- `operator`: disconnect/announcement 가능
- `admin`: runtime settings 포함 전체 가능

## 감사 로그

`/admin`, `/api/v1/*` 요청은 `admin_audit` 구조화 로그를 남긴다.

- `request_id`, `actor`, `role`
- `method`, `path`, `resource`
- `result`, `status_code`, `latency_ms`
- `source_ip`, `timestamp`

## Overview 트렌드

`GET /api/v1/overview`는 `counts` 외에 `audit_trend`를 제공한다.

- `counts.http_server_errors_total`
- `audit_trend.step_ms`, `audit_trend.max_points`
- `audit_trend.points[]` (`timestamp_ms`, `http_errors_total`, `http_server_errors_total`, `http_unauthorized_total`, `http_forbidden_total`)
