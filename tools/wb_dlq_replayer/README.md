# 사후 처리 큐(DLQ) 재처리기(wb_dlq_replayer)

`wb_dlq_replayer`는 write-behind DLQ(Stream)로 떨어진 이벤트를 다시 PostgreSQL에 적재하고, 성공 여부에 따라 DLQ → 본 스트림 또는 DEAD 스트림으로 정리하는 유틸리티다. 장애 복구 시 DLQ를 빠르게 비우고, 재시도 정책을 조정할 때 사용한다.

이 도구가 존재하는 이유는, DLQ를 사람이 직접 손으로 옮기기 시작하면 실수와 누락이 쉽게 생기기 때문이다. 재처리 로직을 별도 도구로 고정해 두면 "어떻게 다시 넣을 것인가"를 매번 임시 스크립트로 만들 필요가 없다.

또한 `wb_dlq_replayer`는 단순 재삽입기가 아니다.

- 성공 항목은 정리한다
- 반복 실패 항목은 DEAD 스트림으로 넘긴다
- 재시도 예산을 둬 무한 재시도를 막는다

즉, 목적은 DLQ를 무조건 비우는 것이 아니라, 재처리 가능한 것과 사람이 개입해야 하는 것을 분리하는 것이다.

```text
tools/wb_dlq_replayer/
├─ main.cpp
└─ README.md
```

## 동작 개요
1. 환경 변수에서 Redis·PostgreSQL 정보를 읽는다.
2. `WB_DLQ_STREAM`에 소비자 그룹(`WB_GROUP_DLQ`)을 만들고, XREADGROUP으로 이벤트를 끊임없이 읽는다.
3. 각 이벤트를 PostgreSQL `session_events` 테이블에 `INSERT ... ON CONFLICT DO NOTHING`으로 반영한다.
4. 성공하면 DLQ ACK 후 종료, 실패하면 백오프/재시도(`WB_RETRY_MAX`, `WB_RETRY_BACKOFF_MS`)를 수행한다.
5. 재시도 한도를 초과하면 DEAD 스트림(`WB_DEAD_STREAM`)으로 이동하고 DLQ에서는 ACK한다.

여기서 중요한 점은 "실패한 항목을 끝없이 붙들지 않는다"는 것이다. 재처리기는 복구 도구여야지, 새 병목이 되어서는 안 된다. 그래서 예산과 DEAD 스트림이 같이 필요하다.

## 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `DB_URI` | PostgreSQL 연결 문자열 | (필수) |
| `REDIS_URI` | Redis 연결 문자열 | (필수) |
| `REDIS_STREAM_KEY` | 원본 write-behind 스트림 | `session_events` |
| `WB_DLQ_STREAM` | DLQ 스트림 이름 | `session_events_dlq` |
| `WB_DEAD_STREAM` | 재시도 한도 초과 시 이동할 스트림 | `session_events_dead` |
| `WB_GROUP_DLQ` | DLQ 소비자 그룹 | `wb_dlq_group` |
| `WB_CONSUMER` | 컨슈머 ID | `wb_dlq_consumer` |
| `WB_RETRY_MAX` | 재시도 횟수 | `5` |
| `WB_RETRY_BACKOFF_MS` | 재시도 간 백오프(ms) | `250` |

`.env`는 개발 편의용 예시 파일이며, 애플리케이션이 자동으로 로드하지 않는다.
로컬에서는 쉘/스크립트에서 `.env`를 로드한 뒤 실행하거나, OS 환경 변수로 직접 주입해야 한다.

## 빌드 및 실행
```powershell
scripts/build.ps1 -Config Debug -Target wb_dlq_replayer
.\build-windows\tools\Debug\wb_dlq_replayer.exe
```

## 운영 팁
- Grafana에서 `wb_dlq_replay` 지표를 모니터링해 재처리 속도를 확인한다.
- DLQ 이벤트가 빠르게 쌓이면 Redis/DB 접근 권한, 스키마 호환성, `wb_worker` 로그를 함께 살펴본다.
- DEAD 스트림은 운영자가 수동 처리해야 하므로 Slack 알람 등과 연동해 즉시 조치하도록 한다.

초보자 기준으로는 다음 순서로 보는 편이 좋다.

1. 왜 DLQ가 쌓였는가
2. 재처리기가 성공적으로 비우는가
3. 반복 실패 항목이 DEAD로 가는가
4. 그 항목을 사람이 분석해야 하는가

이 순서를 지키면 "무조건 다시 밀어 넣기"보다 훨씬 안전하게 장애를 다룰 수 있다.
