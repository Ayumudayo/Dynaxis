# Knights Chat Stack

Knights는 C++20로 작성된 실시간 채팅 서버/게이트웨이/로드밸런서/CLI 클라이언트 묶음입니다. Redis(Streams, Pub/Sub)와 PostgreSQL을 조합해 스냅샷, write-behind, DLQ 복구까지 지원합니다.

## 필수 요구 사항
- CMake ≥ 3.20, MSVC 19.3x+/Clang 14+/GCC 11+
- vcpkg (의존성 관리)
- Redis 6+, PostgreSQL 13+
- .env 파일에 DB_URI, REDIS_URI, METRICS_PORT 등을 정의

## 빠른 빌드 & 실행
`powershell
# Windows PowerShell 예시
scripts/build.ps1 -UseVcpkg -Config Debug -Target server_app
scripts/build.ps1 -UseVcpkg -Config Debug -Target load_balancer_app
scripts/build.ps1 -UseVcpkg -Config Debug -Target gateway_app

# 서버 실행
.uild-msvc\server\Debug\server_app.exe 5000
.uild-msvc\load_balancer\Debug\load_balancer_app.exe
.uild-msvc\gateway\Debug\gateway_app.exe
`

Metrics는 METRICS_PORT(기본 9090)에서 curl http://127.0.0.1:9090/metrics 로 확인할 수 있고, write-behind는 scripts/smoke_wb.ps1로 빠르게 검증합니다.

## 문서 모음
- 시작하기: docs/getting-started.md, docs/build.md, docs/configuration.md
- 설계/로드맵: docs/repo-structure.md, docs/roadmap.md
- 데이터 계층: docs/db/redis-strategy.md, docs/db/write-behind.md
- 운영: docs/ops/deployment.md, docs/ops/gateway-and-lb.md, docs/ops/observability.md, docs/ops/runbook.md, docs/ops/fallback-and-alerts.md
- 테스트: docs/tests.md

## 서브 프로젝트
| 경로 | 설명 |
| --- | --- |
| core/ | 공용 라이브러리(server_core) – Asio 기반 네트워크, job queue, storage 래퍼 |
| server/ | 채팅 서버(server_app) – 방/스냅샷/Redis PubSub/write-behind |
| gateway/ | TCP ↔ gRPC 브리지(gateway_app) |
| load_balancer/ | Consistent Hash + sticky 세션(load_balancer_app) |
| devclient/ | FTXUI 기반 CLI(dev_chat_cli) |
| 	ools/ | write-behind 워커, DLQ 재처리 등 유틸리티 |

각 디렉터리의 README에서 세부 빌드법과 운영 팁을 확인할 수 있습니다. 운영 관련 최신 정보는 docs/ops 폴더를 참고하세요.
