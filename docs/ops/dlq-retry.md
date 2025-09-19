# Write-behind DLQ 재처리 런북

목표: `WB_DLQ_STREAM`에 쌓인 이벤트를 재처리하여 DB(session_events)에 반영한다. 반복 실패 시 `WB_DEAD_STREAM`으로 이동한다.

## 구성
- 스트림
  - DLQ: `WB_DLQ_STREAM` (기본 `session_events_dlq`)
  - DEAD: `WB_DEAD_STREAM` (기본 `session_events_dead`)
- 그룹/컨슈머
  - 그룹: `WB_GROUP_DLQ` (기본 `wb_dlq_group`)
  - 컨슈머: `WB_CONSUMER` (기본 `wb_dlq_consumer`)
- 재시도
  - `WB_RETRY_MAX` (기본 5)

## 실행 바이너리
- 빌드 산출물: `wb_dlq_replayer`
- 환경 변수: `DB_URI`, `REDIS_URI`, `WB_DLQ_STREAM`, `WB_DEAD_STREAM`, `WB_GROUP_DLQ`, `WB_CONSUMER`, `WB_RETRY_MAX`

## 동작 요약
1) DLQ 스트림을 컨슈머 그룹으로 블로킹 읽기(XREADGROUP)
2) 각 항목 처리:
   - `orig_event_id`가 있으면 이를 이벤트 고유키로 사용, 없으면 DLQ 항목 id 사용
   - 원본 필드를 JSON(payload)로 직렬화(`error`,`retry_count` 제외)
   - `insert into session_events(event_id, ...) on conflict do nothing` 멱등 커밋
   - 성공 시 ACK, 로그: `metric=wb_dlq_replay ok=1 event_id=...`
   - 실패 시 `retry_count+1`
     - `retry_count >= WB_RETRY_MAX` → DEAD 스트림으로 이동 후 ACK, 로그: `metric=wb_dlq_replay_dead ...`
     - 아니면 DLQ에 재삽입 후 ACK, 로그: `metric=wb_dlq_replay retry=1 ...`

## 운영 명령 예시
```
# 환경 준비(.env 권장) 후 실행
set DB_URI=postgres://...
set REDIS_URI=redis://127.0.0.1:6379
set WB_DLQ_STREAM=session_events_dlq
set WB_DEAD_STREAM=session_events_dead
set WB_GROUP_DLQ=wb_dlq_group
set WB_CONSUMER=host-a:worker-1
set WB_RETRY_MAX=5

./wb_dlq_replayer
```

## 모니터링 지표(로그)
- `metric=wb_dlq_replay ok=1 event_id=...`
- `metric=wb_dlq_replay retry=1 event_id=... retry_count=N`
- `metric=wb_dlq_replay_dead move=1 event_id=...`

## 주의사항
- DLQ 항목에는 원본 필드와 `orig_event_id`, `error`, 선택적으로 `retry_count`가 포함된다.
- DEAD 스트림은 수동 점검/삭제 정책을 정해 주기적으로 청소한다.
- 재처리 논리는 session_events 스키마를 가정한다. 스키마 변경 시 replayer를 함께 갱신한다.

