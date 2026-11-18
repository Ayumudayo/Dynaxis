# Dynaxis Web Admin Console (`admin_console`)

Dynaxis 스택(`haproxy`, `gateway-1..2`, `server-1..N`, `wb_worker`, `redis`, `postgres`, `migrator`, 선택적 `prometheus`/`grafana`)의 상태를 조회 전용 제어면(v1)으로 통합 관측하는 C++20 백엔드(`admin_app`)와 무빌드 Vanilla ES Modules 웹 UI(`admin_console/web`) 패키지이다.

## 구성
- `include/admin/`, `src/`: `admin_console_core` 정적 라이브러리 및 `admin_app` 실행 파일
  - 사전 등록 서비스 카탈로그(`service_catalog`), 병렬 bounded probe 폴러(`inventory_poller`), HTTP/RESP/Postgres 프로브, Prometheus 메트릭 파서, 전용 HTTP 서버(`admin_http_server`) 및 인증·정적자산·API 라우터(`admin_router`)
- `web/`: 무빌드 정적 자산 (`index.html`, `styles/tokens.css`, `styles/app.css`, `js/api.js`, `js/render.js`, `js/app.js`)

## API v1 엔드포인트
- `GET /healthz`, `GET /readyz`, `GET /metrics`
- `GET /api/v1/meta`: 콘솔 버전(`v1`), 환경 라벨(`LOCAL`/`DEV`/`PROD`), 읽기 전용 여부, 인증 모드, 폴링·TTL 주기, 외부 관측 링크
- `GET /api/v1/overview`: 상단 KPI 집계 및 5종 시맨틱 상태(`ready`, `degraded`, `not_ready`, `completed`, `unknown`)가 반영된 서비스 목록
- `GET /api/v1/services`, `GET /api/v1/services/<id>`: 서비스 목록 및 단건 상세 스냅샷

## 주요 환경 변수
- `ADMIN_LISTEN_PORT` (기본 `9200`), `ADMIN_ENV_LABEL` (`LOCAL`)
- `ADMIN_READ_ONLY` (`true`), `ADMIN_AUTH_MODE` (`token` | `development`)
- `ADMIN_TOKEN`, `ADMIN_OPERATOR_TOKEN`, `ADMIN_VIEWER_TOKEN`
- `ADMIN_WEB_ROOT` (`admin_console/web`), `ADMIN_TOPOLOGY_PATH`, `MIGRATIONS_DIR`
- `ADMIN_TARGET_MODE` (`host` | `docker`), `ADMIN_POLL_INTERVAL_MS` (`5000`), `ADMIN_PROBE_TIMEOUT_MS` (`1500`), `ADMIN_STALE_TTL_MS` (`15000`)

세부 설계와 단계별 로드맵은 [웹 관리 콘솔 설계 문서](../docs/architecture/web-admin-console.md)를 참고한다.
