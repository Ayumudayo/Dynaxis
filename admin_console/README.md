# Dynaxis Web Admin Console (`admin_console`)

Dynaxis 스택(`haproxy`, `gateway-1..2`, `server-1..N`, `wb_worker`, `redis`, `postgres`, `migrator`, 선택적 `prometheus`/`grafana`)의 상태를 통합 관측하고(v1), 채팅방·세션 진실 소스 대조·제재(`mute`/`ban`/`kick`/방 종료)·Write-Behind DLQ 재처리·감사 로그를 제어하며(v2), 허용 목록 기반 비동기 서비스 재시작 및 스키마 기반 설정/롤백(v3)을 수행하는 C++20 백엔드(`admin_app`), 무빌드 Vanilla ES Modules 웹 UI(`admin_console/web`), 호스트 실행 어댑터(`../tools/admin_host_adapter.py`) 패키지이다.

## 구성
- `include/admin/`, `src/`: `admin_console_core` 정적 라이브러리 및 `admin_app` 실행 파일
  - `v1`: 사전 등록 서비스 카탈로그(`service_catalog`), 병렬 bounded probe 폴러(`inventory_poller`), HTTP/RESP/Postgres 프로브, Prometheus 메트릭 파서, 전용 HTTP 서버(`admin_http_server`), 정적자산·API 라우터(`admin_router`)
  - `v2`: PostgreSQL + Redis 채팅 운영 백엔드(`chat_ops_backend`), RESP `PTTL`/`XLEN`/`XRANGE` 스트림 검사(`resp_probe`), 멱등성·제재·감사 스키마(`tools/migrations/0003_admin_control_plane_v2.sql`)
  - `v3`: 비동기 작업 큐(`admin_operations`) 및 스키마 기반 설정 리비전(`admin_config_revisions`) 백엔드(`ops_config_backend`), 마이그레이션(`tools/migrations/0004_admin_control_plane_v3.sql`), 호스트 실행 어댑터(`../tools/admin_host_adapter.py`)
- `web/`: 무빌드 정적 자산 (`index.html`, `styles/tokens.css`, `styles/app.css`, `js/api.js`, `js/render.js`, `js/views_v2.js`, `js/views_v3.js`, `js/app.js`)

## API v1 / v2 / v3 엔드포인트
- `GET /healthz`, `GET /readyz`, `GET /metrics`
- `GET /api/v1/meta`, `GET /api/v1/overview`, `GET /api/v1/services`, `GET /api/v1/services/<id>`
- `GET /api/v1/chat/rooms`, `GET /api/v1/chat/rooms/<room>`, `POST /api/v1/chat/rooms/<room>/close` (`operator`+)
- `GET /api/v1/chat/users`, `GET /api/v1/chat/users/<subject>`, `POST /api/v1/chat/users/<subject>/kick` (`operator`+)
- `GET /api/v1/chat/sanctions`, `POST /api/v1/chat/sanctions`, `DELETE /api/v1/chat/sanctions/<id>` (`operator`+)
- `GET /api/v1/wb/dlq`, `POST /api/v1/wb/dlq/replay` (`operator`+)
- `GET /api/v1/audit-logs`
- `GET /api/v1/operations`, `GET /api/v1/operations/<id>`, `POST /api/v1/operations` (`admin` 전용)
- `POST /api/v1/operations/worker/claim`, `POST /api/v1/operations/<id>/worker-result` (`admin` 전용, 호스트 어댑터용)
- `GET /api/v1/config`, `POST /api/v1/config/apply`, `POST /api/v1/config/rollback` (`admin` 전용 쓰기)

## 주요 환경 변수
- `ADMIN_LISTEN_PORT` (기본 `9200`), `ADMIN_ENV_LABEL` (`LOCAL`)
- `ADMIN_READ_ONLY` (`true`), `ADMIN_AUTH_MODE` (`token` | `development`), `ADMIN_DEV_ROLE` (`viewer` | `operator` | `admin`)
- `ADMIN_TOKEN`, `ADMIN_OPERATOR_TOKEN`, `ADMIN_VIEWER_TOKEN`
- `ADMIN_WEB_ROOT` (`admin_console/web`), `ADMIN_TOPOLOGY_PATH`, `MIGRATIONS_DIR`
- `REDIS_URI`, `REDIS_CHANNEL_PREFIX` (`dynaxis:`), `REDIS_STREAM_KEY`, `WB_DLQ_STREAM`, `WB_DEAD_STREAM`, `WB_GROUP_DLQ`, `WB_RETRY_MAX`
- `ADMIN_TARGET_MODE` (`host` | `docker`), `ADMIN_POLL_INTERVAL_MS` (`5000`), `ADMIN_PROBE_TIMEOUT_MS` (`1500`), `ADMIN_STALE_TTL_MS` (`15000`), `ADMIN_OP_VERIFY_TIMEOUT_MS` (`30000`)

세부 설계와 단계별 로드맵은 [웹 관리 콘솔 설계 문서](../docs/architecture/web-admin-console.md)를 참고한다.
