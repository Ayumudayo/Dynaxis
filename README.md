# Server Project — 개요 및 빠른 시작

본 저장소는 C++20 기반의 서버 애플리케이션과 부속 워커/도구/문서를 포함합니다. Redis(Pub/Sub, Streams)와 PostgreSQL을 활용해 채팅/프레즌스/브로드캐스트/이벤트 적재를 제공합니다.

## 빠른 시작
- 필수: CMake 3.20+, MSVC 19.3x+/GCC 11+/Clang 14+, vcpkg, Redis, PostgreSQL
- .env 작성: DB_URI, REDIS_URI 등 — 예시는 `docs/getting-started.md`
- 빌드/실행(Windows PowerShell 예시)
  - `scripts/build.ps1 -UseVcpkg -Config Debug -Target server_app`
  - `scripts/build.ps1 -UseVcpkg -Config Debug -Target wb_worker`
  - 서버 실행: `build-msvc/server/Debug/server_app.exe 5000`
  - 워커 실행: `build-msvc/Debug/wb_worker.exe`
- 스모크 테스트: `scripts/smoke_wb.ps1`
- 지표 노출: `METRICS_PORT=9090` 설정 후 `curl http://127.0.0.1:9090/metrics`

## 문서 바로가기
- 설정/실행: `docs/getting-started.md`, `docs/build.md`, `docs/configuration.md`
- 로드맵: `docs/roadmap.md`
- Redis 전략/Write-behind: `docs/db/redis-strategy.md`, `docs/db/write-behind.md`
- 관측성: `docs/ops/observability.md`, `docs/ops/dlq-retry.md`, `docs/ops/gateway-and-lb.md`
- 배포 전략: `docs/ops/deployment.md`
- 저장소 구조: `docs/repo-structure.md`
- 테스트 가이드: `docs/tests.md`

## 구성 요소
- `server_app`: 테스트용 서버(채팅/프레즌스/브로드캐스트)
- `wb_worker`: Redis Streams → Postgres 배치 커밋 워커
- `wb_dlq_replayer`: DLQ 재처리 도구(재시도/데드 이동)
- `wb_emit`/`wb_check`: 스모크 보조 도구
- `dev_chat_cli`: FTXUI 기반 개발용 클라이언트

## 라이선스
- 리포지터리 라이선스/저작권 정책은 상위 정책을 따릅니다. 별도 표기가 없는 한 외부 배포를 전제로 하지 않습니다.
