# Redis Write-Behind for Session Activities

목표: 세션에서 발생하는 고빈도 이벤트를 Redis에 임시 저장하고, 주기적으로(또는 임계 도달 시) RDB(PostgreSQL)에 배치 커밋하여 RDB I/O를 줄이고 지연을 낮춘다.

## 현재 상태
- 라이브러리: redis-plus-plus 1.3.15(vcpkg, x64-windows)
- 구현: 클라이언트 레벨 Streams API(XGROUP CREATE mkstream, XADD, XREADGROUP(block/limit), XACK) 제공
  - 파일: `server/src/storage/redis/client.cpp`
- 워커: `.env` 인식 추가, 컨슈머 그룹으로 읽고 ACK하는 스켈레톤 동작
  - 파일: `tools/wb_worker/main.cpp`
- 미연결: 서버의 이벤트 생산(XADD) 경로, Postgres 배치 커밋/멱등 처리/DLQ/메트릭은 이후 단계

## 패턴 개요
- 수집(ingest): 서버는 세션 이벤트를 Redis에 적재
  - 권장: Redis Streams (`stream:session_events`) + 컨슈머 그룹
  - 대안: List(단순 큐). 내구/재처리/스케일 측면에서 Streams가 우수
- 처리(process): 워커가 배치 단위로 이벤트를 읽어 Postgres에 트랜잭션 커밋
- 커밋 전략: 시간 기반(max delay) + 개수/바이트 기반 임계치 동시 적용
- 내구성: Redis AOF everysec + Streams 보존 정책으로 유실 위험 최소화

## 이벤트 모델
- 타입 예시:
  - `session_login`, `session_logout`, `presence_heartbeat`, `room_join`, `room_leave`, `message_ack`, `typing_start/stop`
- 공통 필드:
  - `event_id`(Redis XADD ID), `ts`, `user_id`, `session_id`, `room_id?`, `payload`
  - 세션 식별자(session_id)는 서버 측에서 세션별 UUID v4를 생성하여 사용한다(내부 숫자 세션 식별자와 분리됨)
- 멱등성 키: `idempotency_key` 또는 `event_id`를 RDB에 기록하여 중복 삽입 방지

## 배치 커밋
- 트랜잭션 범위: 동일 테이블/유형끼리 묶거나, 다중 테이블 시 단일 트랜잭션
- 정책:
  - `BATCH_MAX_EVENTS`(예: 100–500)
  - `BATCH_MAX_BYTES`(예: 512 KiB–4 MiB)
  - `BATCH_MAX_DELAY_MS`(예: 500–3000ms). 기본값 500ms로 사실상 준실시간 반영.
- 실패 처리: 전체 롤백 후 재시도(지수 백오프). 부분 실패는 레코드 단위로 분리 재시도 또는 DLQ로 이동

## 전달 보장
- 모드: at-least-once(현실적 선택). 멱등 삽입으로 중복 방지
- Exactly-once는 비용/복잡도가 큼. 필요 시 테이블별 고유 제약과 이벤트 오프셋 체크포인트로 근접 구현

## 장애/재시작
- 워커는 Streams 컨슈머 그룹의 펜딩(pending) 항목을 재처리하여 손실을 방지
- 서버 재시작: 미커밋 이벤트는 Streams에 남아 재처리 대상이 됨
- Redis 장애: 폴백으로 동기 기록(write-through) 전환 또는 기능 축소(설정 플래그)

## 데이터 모델(예)
- `session_events`(append-only) — 이벤트소싱/감사 목적
  - `id bigserial`, `event_id text unique`, `type text`, `ts timestamptz`, `user_id uuid`, `session_id uuid`, `room_id uuid`, `payload jsonb`
  - 인덱스: `user_id, ts`, `session_id, ts`, `type, ts`
- 또는 `aggregates` — 압축된 스냅샷(예: presence 카운트). 이벤트는 TTL 후 정리 가능

## 일관성 등급
- 클라이언트 관점: 최종적 일관성(Eventual). 쓰기 후 즉시 읽기는 Redis 캐시(세션/프레즌스)로 충족
- RDB는 약간 뒤늦게 반영(T+Δ). 감사/리포팅/복구용

## 구성 키
- `WRITE_BEHIND_ENABLED` (bool)
- `REDIS_STREAM_KEY=session_events`, `REDIS_STREAM_MAXLEN`(approx trim)
- `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_MAX_DELAY_MS`
- `WB_WORKER_CONCURRENCY`, `WB_RETRY_BACKOFF_MS`
- `WB_DLQ_STREAM=session_events_dlq`(옵션)
- `WB_DLQ_ON_ERROR=1`(에러 시 DLQ로 포워드; 0이면 비활성)
- `WB_ACK_ON_ERROR=1`(에러 시에도 ACK; 0이면 ACK 생략해 재시도 유도)

## Streams 운영 키/그룹/필드 정리
- 생산 키: `session_events`(=`REDIS_STREAM_KEY`)
- 소비 키: `session_events`(동일 스트림, 컨슈머 그룹 기반)
- 그룹: `wb_group`(예시, 설정 키 `WB_GROUP`로 주입)
- 컨슈머: 워커 인스턴스 식별자(예: `host-1:pid`), 설정 키 `WB_CONSUMER`
- 필드 집합(권장 최소):
  - `type`(예: `session_login` 등), `ts`, `user_id`, `session_id`, `room_id?`, `payload`
  - 멱등 보강 시: `idempotency_key` 포함 고려
- 기타 운영 키:
  - `WRITE_BEHIND_ENABLED`: 기능 토글
  - `REDIS_STREAM_KEY`: 스트림 키 이름
  - `WB_GROUP`: 컨슈머 그룹명(예: `wb_group`)
- `WB_CONSUMER`: 소비자 이름 접두 또는 전체명
- `REDIS_STREAM_MAXLEN`: XADD 시 approx trim 임계

## 서버 생산자 경로(계획/적용 위치)
- 게이트: `WRITE_BEHIND_ENABLED`가 true일 때만 Streams로 XADD 수행. 키는 `REDIS_STREAM_KEY`(기본 `session_events`).
- 트림: `REDIS_STREAM_MAXLEN`가 설정되어 있으면 `XADD MAXLEN ~ <count>` 적용.
- 이벤트 매핑(파일/함수 기준):
  - 로그인 성공 → `session_login`
    - 파일: `server/src/chat/handlers_login.cpp`
    - 타이밍: 유저 식별자(UID) 확정 및 상태 갱신 후
    - 필드 예: `type=session_login`, `ts`, `user_id`, `session_id`, `room_id=lobby`, `payload={ip}`
  - 룸 입장 → `room_join`
    - 파일: `server/src/chat/handlers_join.cpp`
    - 타이밍: `state_.cur_room`/`rooms` 갱신 직후
    - 필드 예: `type=room_join`, `ts`, `user_id`, `session_id`, `room_id`
  - 룸 퇴장 → `room_leave`
    - 파일: `server/src/chat/handlers_leave.cpp`
    - 타이밍: 방 멤버셋에서 제거 직후(로비 이동 전/후 중 한 지점으로 표준화)
    - 필드 예: `type=room_leave`, `ts`, `user_id`, `session_id`, `room_id`
  - 세션 종료 → `session_close`
    - 필드 해석: 공통 필드는 `type`, `ts_ms`, `session_id`, `user_id`, `room_id`, `gateway_id`; 상황별로 `ip`, `prev_room`, `next_room` 등을 추가합니다.
    - 파일: `server/src/chat/session_events.cpp`
    - 타이밍: 상태 정리 및 브로드캐스트 직후
    - 필드 예: `type=session_close`, `ts`, `user_id?`, `session_id`, `room_id?`
- 향후 후보(필요 시 도입):
  - 채팅 송신 이벤트 요약 또는 ACK(`handlers_chat.cpp`) → 메시지 영속화와의 관계를 고려해 선택
  - 타이핑 시작/중지(`typing_start/stop`) → 저중요 이벤트로 write-behind 1단계 도입에 적합
- 멱등성: `idempotency_key`(예: `session_id + ts + type`) 또는 Streams ID 기반 처리. DB 고유 제약으로 중복 차단.

## 현재 워커 동작(구현됨)
- 배치 파라미터 적용: `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_DELAY_MS`
- 멱등성: `event_id`(Streams ID) 고유 제약으로 중복 삽입 무해
- 부분 커밋: 이벤트별 개별 트랜잭션으로 성공분만 커밋/ACK
- 에러 처리:
  - `WB_DLQ_ON_ERROR=1`이고 `WB_DLQ_STREAM`이 지정되면 DLQ로 포워드 후 ACK
  - DLQ 전송도 실패하면 ACK 생략(재시도)
  - `WB_ACK_ON_ERROR=1`이면 DLQ 비활성이어도 에러 항목을 ACK하여 워크플로 차단 방지
- 펜딩 길이 관측: `metric=wb_pending value=<n>` 주기(1s) 로그
 - UUID 정규화: `user_id/session_id/room_id`가 UUID 형식이 아니면 NULL 저장(워커에서 정규화)

## 다음 과제(TODO)
- DLQ 메타: retry_count/last_error_ts 부여, 재처리 워커/런북
- 트림: `REDIS_STREAM_MAXLEN`에 따른 approx trim 적용(`XADD MAXLEN ~`)
- 관측성: `wb_batch_size`, `wb_commit_ms`, `wb_fail_total`, `wb_pending`, `wb_dlq_total` 등 메트릭 수집

## DLQ 재처리 도구(wb_dlq_replayer)
- 동작: DLQ에서 읽어 멱등 커밋, 실패 시 재시도/데드 이동
- 설정: `WB_DLQ_STREAM`, `WB_DEAD_STREAM`, `WB_GROUP_DLQ`, `WB_CONSUMER`, `WB_RETRY_MAX`, `WB_RETRY_BACKOFF_MS`
- 백오프: 재시도마다 2^n 배(상한 10초) 지연 후 재삽입
- 로그: `metric=wb_dlq_replay*` 계열(성공/재시도/데드)

## 모니터링
- 레이턴시 p50/p95, 배치 크기, 커밋율, 실패율, 재시도/펜딩 길이, DLQ 길이

## 점진 도입 전략
- 1단계: write-through + cache-aside(기본)
- 2단계: 비핵심 이벤트부터 write-behind(typing/presence/acks)
- 3단계: 중요 이벤트 확대, 멱등 보강/알람 확립
- 4단계: 필요 시 Streams를 통한 내구 브로드캐스트로 통합

## 트레이드오프
- 장점: RDB 부하 감소, 지연 개선, 스파이크 흡수
- 단점: 최종적 일관성, 운영 복잡도 증가, Redis 의존 강화 → 장애 대비 폴백/알람 필수
