# Redis Write-Behind for Session Activities

목표: 세션에서 발생하는 고빈도 이벤트를 Redis에 임시 저장하고, 주기적으로(또는 임계 도달 시) RDB(PostgreSQL)에 배치 커밋하여 RDB I/O를 줄이고 지연을 낮춘다.

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
- 멱등성 키: `idempotency_key` 또는 `event_id`를 RDB에 기록하여 중복 삽입 방지

## 배치 커밋
- 트랜잭션 범위: 동일 테이블/유형끼리 묶거나, 다중 테이블 시 단일 트랜잭션
- 정책:
  - `BATCH_MAX_EVENTS`(예: 500)
  - `BATCH_MAX_BYTES`(예: 1–4 MiB)
  - `BATCH_MAX_DELAY_MS`(예: 100–500ms)
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
- `WB_DLOUT_STREAM=session_events_dlq`(옵션)

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

