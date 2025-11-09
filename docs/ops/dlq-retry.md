# DLQ 재처리 가이드 (상세)

write-behind 워커(`wb_worker`)가 실패한 이벤트를 DLQ 로 보내면, `wb_dlq_replayer` 가 재처리한다. 본 문서는 구조, 설정, 모니터링, 트러블슈팅을 모두 다룬다.

## 1. DLQ 구조
| 키 | 설명 |
| --- | --- |
| `wb:stream:<name>` | 정상 스트림 (예: `wb:stream:session_events`)
| `wb:dlq:<name>` | 실패 이벤트 보관. 필드: `payload`, `retry_count`, `last_error`, `created_at`
| `wb:dlq:dead:<name>` | `retry_count >= WB_RETRY_MAX` 인 항목을 이동

각 항목에는 원본 이벤트 ID(`orig_id`) 와 마지막 오류 메시지가 포함돼야 한다.

## 2. 재처리 절차
```bash
# 환경 변수 준비	export DB_URI=postgres://...
export WB_STREAM=session_events
export WB_DLQ_STREAM=session_events_dlq
export WB_RETRY_MAX=5
export WB_RETRY_BACKOFF_MS=500

./wb_dlq_replayer --once
```
1. DLQ에서 가장 오래된 항목을 읽는다.
2. `retry_count < WB_RETRY_MAX` 이면 작업 수행 → 성공 시 ACK + 로그 `metric=wb_dlq_replay ok=1 event_id=`
3. 실패하면 `retry_count++`, `WB_RETRY_BACKOFF_MS` 후 스트림에 재삽입 → 로그 `metric=wb_dlq_replay retry=1`
4. 기준치를 넘으면 `wb:dlq:dead` 로 이동 → `metric=wb_dlq_replay_dead move=1`

## 3. 구성 옵션
| 변수 | 설명 | 기본 |
| --- | --- | --- |
| `WB_DLQ_STREAM` | DLQ 스트림 이름 | `<STREAM>_dlq` |
| `WB_RETRY_MAX` | 재시도 횟수 | `5` |
| `WB_RETRY_BACKOFF_MS` | 재시도 대기 | `250` |
| `WB_DLQ_BATCH` | 한 번에 처리할 항목 수 | `50` |
| `DB_URI` | PostgreSQL 연결 | (필수) |

## 4. 모니터링
- `metric=wb_dlq_replay ok=1/ retry=1 / dead=1`
- Prometheus 규칙 예시: `sum(increase(wb_dlq_replay_dead_total[5m])) > 0`
- Grafana 패널: DLQ backlog = `XLEN wb:dlq:<stream>`

## 5. 트러블슈팅
| 증상 | 조치 |
| --- | --- |
| DLQ backlog 급증 | ① 최근 배포 rollback ② 오류 로그에서 SQL/네트워크 원인 확인 |
| dead 스트림 증가 | 데이터 손상 여부 확인 후 수동 patch 또는 삭제 |
| replayer crash | `WB_RETRY_MAX` 값을 임시로 줄이고, 문제가 되는 이벤트 ID 추적 |

## 6. FAQ
- **Q. DLQ를 비우지 않고도 서버를 계속 운영할 수 있나?** → 가능하지만 데이터 정합성을 위해 24시간 내 처리 권장.
- **Q. DLQ 항목을 수동 수정해도 되나?** → payload 가 JSON이면 수동 patch 가능하나, 변경 이력을 남기고 재처리 결과를 꼭 확인한다.

## 7. 참고
- `tools/wb_worker/README.md`
- `docs/db/write-behind.md`
- `docs/ops/runbook.md`
