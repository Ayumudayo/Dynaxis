# wb_worker

`wb_worker`는 Redis Streams에 적재된 write-behind 이벤트를 읽어 PostgreSQL 등 영속 저장소에 반영하는 배치 워커입니다. 채팅 서버(`server_app`)가 `WRITE_BEHIND_ENABLED=1`로 실행되면 로그인/방 입장/퇴장/세션 종료와 같은 이벤트가 `REDIS_STREAM_KEY` 스트림에 발행되고, 워커가 이를 소비해 DB에 커밋합니다.

```text
tools/wb_worker/
├─ main.cpp
└─ README.md
```

## 동작 개요
- `.env` 로드: 실행 파일과 동일한 경로 또는 저장소 루트의 `.env`를 읽어 Redis/DB 설정을 공유합니다.
- Redis Streams 소비: `WB_GROUP`, `WB_CONSUMER`로 컨슈머 그룹을 구성하고 `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_DELAY_MS` 조건에 맞춰 배치를 읽어옵니다.
- DB 커밋: 이벤트를 파싱해 PostgreSQL 테이블(예: `session_events`)에 저장하고 실패 시 `WB_DLQ_STREAM`으로 이벤트를 이동합니다.
- 메트릭/로그: `wb_commit_ms`, `wb_fail_total`, `wb_pending` 등 운영 지표를 로그·메트릭으로 남깁니다.

## 필수 환경 변수
| 변수 | 설명 | 비고 |
| --- | --- | --- |
| `REDIS_URI` | Redis 연결 URI | 서버와 동일 값 사용 권장 |
| `REDIS_STREAM_KEY` | 기본 Streams 이름 | 예: `session_events` |
| `WB_GROUP` / `WB_CONSUMER` | 컨슈머 그룹/ID | 존재하지 않으면 워커가 생성 |
| `WB_BATCH_MAX_EVENTS` | 배치 최대 이벤트 수 | 기본 100 (예시) |
| `WB_BATCH_MAX_BYTES` | 배치 최대 바이트 | 기본 524288 |
| `WB_BATCH_DELAY_MS` | 배치 지연(ms) | 기본 500 |
| `WB_DLQ_STREAM` | 실패 이벤트 DLQ 스트림 | DLQ 운용 시 필수 |

추가로 PostgreSQL 커밋을 위해 `DB_URI`, `DB_POOL_MIN/MAX` 등을 함께 설정해야 합니다.

## 빌드 및 실행
```powershell
cmake --build build-msvc --target wb_worker
.\build-msvc\Debug\wb_worker.exe
```

`scripts/build.ps1 -Target wb_worker` 또는 `scripts/run_all.ps1`을 사용하면 `server_app`과 함께 빌드·실행할 수 있습니다.

## 테스트 & 보조 도구
- `scripts/smoke_wb.ps1`: 워커를 백그라운드로 띄운 뒤 `wb_emit`/`wb_check`로 end-to-end 검증
- `tools/wb_emit`: 샘플 이벤트를 Streams에 XADD
- `tools/wb_check`: DB에 반영된 레코드를 조회
- `tools/wb_dlq_replayer`: DLQ 스트림 재처리 유틸리티

## 추가 문서
- `docs/db/write-behind.md`
- `docs/ops/runbook.md`
