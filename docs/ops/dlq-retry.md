# DLQ 재처리 가이드

write-behind 워커는 실패한 이벤트를 DLQ(Dead Letter Queue)에 적재한다. 이 문서는 DLQ에 쌓인 메시지를 재처리하는 방법과 운영 시 주의사항을 설명한다.

## 1. DLQ 구조
- Redis Streams 키: `wb:dlq:<stream>`  
- 필드: `payload`(JSON), `retry_count`, `last_error`  
- `retry_count` 가 `WB_RETRY_MAX` 를 초과하면 dead 스트림(`wb:dlq:dead`)으로 이동한다.

## 2. 재처리 프로세스
1. `wb_dlq_replayer` 실행  
   ```bash
   WB_RETRY_MAX=5 \
   DB_URI=postgres://app:example@db:5432/appdb \
   ./wb_dlq_replayer
   ```
2. 최신 DLQ 항목을 읽고 DB/외부 시스템에 다시 기록한다.  
3. 재처리 성공 시 ACK 후 로그: `metric=wb_dlq_replay ok=1 event_id=<id>`  
4. 실패 시 `retry_count` 를 증가시키고 DLQ에 다시 삽입, 로그: `metric=wb_dlq_replay retry=1 ...`  
5. `retry_count >= WB_RETRY_MAX` 이면 dead 스트림으로 이동, 로그: `metric=wb_dlq_replay_dead move=1 ...`

## 3. 운영 체크리스트
- `wb_pending` 지표를 통해 실시간 적체량을 감시한다.  
- DLQ 재처리 전에는 애플리케이션 오류(스키마 변경, 외부 API 문제)를 먼저 해결해야 한다.  
- dead 스트림은 수동 점검 후 롤백 또는 데이터 패치를 진행한다.  
- runbook에는 DLQ 재처리 절차와 “언제 중단할 것인지” 기준을 명시한다.

## 4. FAQ
- **Q. DLQ가 급격히 쌓일 때?**  
  A. write-behind 워커 로그에서 `wb_fail_total` 변화를 확인하고, 같은 시각의 애플리케이션 에러/DB 에러와 상관관계를 찾는다.
- **Q. 재처리 중 또 실패한다면?**  
  A. `last_error` 필드를 참고해 idempotent 한지 검토 후, 필요 시 수동으로 payload 를 수정한다.

## 5. 참고
- 워커 메트릭: `tools/wb_worker/main.cpp`  
- Observability: `docs/ops/observability.md`  
- Runbook: `docs/ops/runbook.md`
