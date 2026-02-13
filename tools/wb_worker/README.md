# wb_worker

`wb_worker`는 Redis Streams로 발행되는 write-behind 이벤트를 읽어 PostgreSQL에 적재하는 콘솔 애플리케이션이다. 채팅 서버(`server_app`)에서 `WRITE_BEHIND_ENABLED=1`이 설정되면 로그인, 룸 입장/퇴장, 세션 종료 같은 이벤트가 Redis Stream으로 기록되고, `wb_worker`가 이를 배치 단위로 DB에 반영한다.

```text
tools/wb_worker/
├─ main.cpp
└─ README.md
```

## 동작 개요
1. 실행 시 환경 변수에서 Redis·PostgreSQL 설정을 로드한다.
2. `WB_GROUP` / `WB_CONSUMER` 이름으로 Redis Stream 소비자 그룹을 생성한다.
3. `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_DELAY_MS` 조건을 만족할 때마다 이벤트를 묶어 PostgreSQL에 INSERT/UPSERT 한다.
4. 처리 실패 시 DLQ 스트림(`WB_DLQ_STREAM`)으로 이동하고, 재처리 정책(`WB_RETRY_MAX`, `WB_RETRY_BACKOFF_MS`)을 따른다.

## 필수 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `REDIS_URI` | Redis 연결 문자열 | (필수) |
| `REDIS_STREAM_KEY` | write-behind 스트림 이름 | `session_events` |
| `WB_GROUP` / `WB_CONSUMER` | 소비자 그룹 / 컨슈머 ID | `wb_group` / `wb_consumer` |
| `WB_BATCH_MAX_EVENTS` | 배치 최대 이벤트 수 | `100` |
| `WB_BATCH_MAX_BYTES` | 배치 최대 바이트 수 | `524288` |
| `WB_BATCH_DELAY_MS` | 배치 지연 시간(ms) | `500` |
| `WB_DLQ_STREAM` | DLQ 스트림 이름 | `session_events_dlq` |
| `WB_RETRY_MAX`, `WB_RETRY_BACKOFF_MS` | 재시도 횟수·백오프(ms) | `5`, `250` |
| `DB_URI` | PostgreSQL 연결 문자열 | (필수) |

`.env`는 개발 편의용 예시 파일이며, 애플리케이션이 자동으로 로드하지 않는다.
로컬에서는 쉘/스크립트에서 `.env`를 로드한 뒤 실행하거나, OS 환경 변수로 직접 주입해야 한다.

## 빌드 및 실행
```powershell
scripts/build.ps1 -Config Debug -Target wb_worker
.\build-windows\tools\Debug\wb_worker.exe
```

또는 `scripts/build.ps1 -Target wb_worker`를 사용해 다른 모듈과 함께 빌드할 수 있다.

## 테스트 & 운영 유틸리티
- `scripts/smoke_wb.ps1` : write-behind 파이프라인을 통째로 점검하는 스모크 스크립트
- `tools/wb_emit` : Redis Stream에 테스트 이벤트를 수동 발행
- `tools/wb_check` : PostgreSQL DB 상태를 조회해 적재 결과를 확인
- `tools/wb_dlq_replayer` : DLQ 스트림을 재처리 가능한 포맷으로 변환

write-behind 설계와 운영 전략에 대한 자세한 설명은 `docs/db/write-behind.md`, `docs/ops/runbook.md`를 참고한다.
