# 빠른 시작 가이드(Setup & Run)

이 문서는 로컬 개발 환경에서 서버/워커를 실행하고, 통합 스모크 테스트까지 수행하는 최소 절차를 제공합니다.

## 요구 사항
- Windows 10/11 또는 Linux(WSL 포함)
- CMake 3.20+, C++20 컴파일러(MSVC 19.3x+/GCC 11+/Clang 14+)
- vcpkg(권장) — 루트의 `vcpkg.json` 매니페스트 사용
- Redis, PostgreSQL (로컬 또는 원격 인스턴스)

## 1) 의존성 설치(권장: vcpkg)
- VCPKG_ROOT 환경변수 설정(예: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg`)
- 첫 빌드시 스크립트가 자동으로 `vcpkg install`을 수행합니다.

## 2) 환경 변수(.env) 준비
프로젝트 루트에 `.env` 파일을 만들고 다음 예시를 기반으로 값을 설정합니다.

```
DB_URI=postgres://user:pass@127.0.0.1:5432/appdb
REDIS_URI=redis://127.0.0.1:6379

# Optional: Pub/Sub 분산 브로드캐스트
USE_REDIS_PUBSUB=1
GATEWAY_ID=gw-local
REDIS_CHANNEL_PREFIX=knights:

# Presence
PRESENCE_TTL_SEC=30
PRESENCE_CLEAN_ON_START=0

# Write-behind
WRITE_BEHIND_ENABLED=1
REDIS_STREAM_KEY=session_events
REDIS_STREAM_MAXLEN=10000
WB_GROUP=wb_group
WB_CONSUMER=wb_consumer
WB_DLQ_STREAM=session_events_dlq
WB_ACK_ON_ERROR=1
WB_DLQ_ON_ERROR=1

# DLQ 재처리
WB_GROUP_DLQ=wb_dlq_group
WB_DEAD_STREAM=session_events_dead
WB_RETRY_MAX=5
WB_RETRY_BACKOFF_MS=250

# Metrics
METRICS_PORT=9090
```

PostgreSQL에 `docs/db/migrations/0004_session_events.sql.md`의 테이블을 적용해 둡니다(session_events 등).

## 3) 빌드
Windows PowerShell:

```
scripts/build.ps1 -UseVcpkg -Config Debug -Target server_app
scripts/build.ps1 -UseVcpkg -Config Debug -Target wb_worker
```

산출물 예시(Visual Studio 제너레이터):
- 서버: `build-msvc/server/Debug/server_app.exe`
- 워커: `build-msvc/Debug/wb_worker.exe`

## 4) 실행
터미널 1 — 서버:
```
build-msvc/server/Debug/server_app.exe 5000
```

터미널 2 — 워커:
```
build-msvc/Debug/wb_worker.exe
```

옵션: DLQ 재처리도 구동하려면 `build-msvc/Debug/wb_dlq_replayer.exe` 실행.

## 5) 통합 스모크 테스트
PowerShell 스크립트 하나로 Streams→DB 반영 경로를 확인합니다.

```
scripts/smoke_wb.ps1 -Config Debug -BuildDir build-msvc
```

내부 동작:
- wb_worker를 백그라운드 기동 → wb_emit로 샘플 이벤트 XADD → wb_check로 Postgres 반영 확인 → wb_worker 종료

## 6) 관측(Observability)
- 서버 메트릭: `METRICS_PORT` 지정 시 `/metrics` 텍스트 포맷 노출
  - 예: `curl http://127.0.0.1:9090/metrics`
  - 노출 예시: `chat_subscribe_total`, `chat_self_echo_drop_total`, `chat_subscribe_last_lag_ms`
- 키=값 로그: 서버/워커/DLQ 재처리에서 최소 지표가 로그로 기록됩니다.

## 문제 해결(Troubleshooting)
- 컴파일 오류(이스케이프/경로): 최신 MSVC/Boost 권장, `vcpkg.json` 기반으로 빌드
- Redis/DB 연결 실패: `.env`와 실제 인스턴스 주소/권한 확인
- Streams 처리 지연: `wb_pending`(XPENDING) 로그 값과 Redis 부하 확인

