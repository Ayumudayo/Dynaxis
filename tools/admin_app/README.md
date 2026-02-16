# admin_app

`admin_app`은 운영 관리 콘솔을 위한 read-only control-plane 스켈레톤이다.

현재 제공 endpoint:

- `/metrics`
- `/healthz`, `/readyz`
- `/admin` (browser UI, source: `tools/admin_app/admin_ui.html`)
- `/api/v1/overview` (JSON)
- `/api/v1/instances` (JSON)
- `/api/v1/instances/{instance_id}` (JSON)
- `/api/v1/sessions/{client_id}` (JSON)
- `/api/v1/worker/write-behind` (JSON)
- `/api/v1/metrics/links` (JSON)

## 환경 변수

| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `METRICS_PORT` | admin HTTP 포트 | `39200` |
| `ADMIN_POLL_INTERVAL_MS` | 백그라운드 상태 수집 주기(ms) | `1000` |
| `REDIS_URI` | Redis 연결 문자열(인스턴스/세션 조회용) | (unset) |
| `SERVER_REGISTRY_PREFIX` | 인스턴스 레지스트리 prefix | `gateway/instances/` |
| `GATEWAY_SESSION_PREFIX` | 세션 디렉터리 key prefix | `gateway/session/` |
| `WB_WORKER_METRICS_URL` | wb_worker metrics URL | `http://127.0.0.1:39093/metrics` |
| `GRAFANA_BASE_URL` | Grafana base URL 링크 | `http://127.0.0.1:33000` |
| `PROMETHEUS_BASE_URL` | Prometheus base URL 링크 | `http://127.0.0.1:39090` |

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
