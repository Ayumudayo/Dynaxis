# 세션 이벤트용 Redis Write-Behind

이 문서는 Dynaxis가 왜 세션 이벤트를 곧바로 Postgres에 쓰지 않고, Redis Streams를 거쳐 write-behind로 적재하는지 설명한다. 핵심 목표는 단순한 "성능 최적화"가 아니다. 서버 hot path에서 데이터베이스 지연을 분리하고, 스파이크를 흡수하고, 장애 시 재처리 경로를 확보하는 것이 목적이다.

세션 이벤트는 양이 많고, 대부분은 사용자 체감 지연과 직접 연결되는 경로에서 발생한다. 이런 이벤트를 매번 동기식으로 RDB에 기록하면 다음 문제가 커진다.

- 로그인/입장/퇴장 같은 핫패스가 DB 응답 시간에 직접 묶인다.
- 순간 트래픽 급증이 곧바로 DB 포화로 이어진다.
- DB 장애가 발생했을 때 서버가 이벤트 생성 자체를 멈추거나, 반대로 유실을 감수해야 하는 양자택일에 몰린다.

Dynaxis의 write-behind는 이 문제를 풀기 위해 두 단계를 둔다.

- `server_app`
  - 이벤트를 Redis Stream에 발행한다.
- `wb_worker`
  - Stream을 소비해 배치 단위로 Postgres에 반영한다.

이렇게 나누면 서버는 짧은 경로에서 이벤트를 내보내고, 워커는 DB 상태에 맞춰 재시도·배치·DLQ·reclaim 정책을 운영할 수 있다. 즉, 사용자 지연과 영속화 비용을 분리하는 구조다.

## 현재 상태
- 라이브러리: redis-plus-plus 1.3.15 (Conan2 lockfile 기준)
- Redis 클라이언트: Streams API(XGROUP CREATE MKSTREAM, XADD, XREADGROUP, XPENDING, XACK, XAUTOCLAIM) 제공
  - 파일: `server/src/storage/redis/client.cpp`, `server/include/server/storage/redis/client.hpp`
- 생산자(server_app): 세션/룸 이벤트를 Redis Stream으로 발행
  - 구현: `ChatService::emit_write_behind_event()`
  - 파일: `server/src/chat/chat_service_core.cpp`, `server/src/chat/session_events.cpp`, `server/src/chat/handlers_*.cpp`
- 워커(wb_worker): 컨슈머 그룹으로 읽고, 배치 트랜잭션으로 Postgres에 적재 후 ACK
  - PEL reclaim: 주기적으로 `XAUTOCLAIM`으로 pending을 회수(옵션)
  - 실패 처리: DLQ(`WB_DLQ_STREAM`) 포워드(옵션) + ACK 정책(`WB_ACK_ON_ERROR`)
  - 메트릭: `METRICS_PORT`가 설정되면 `/metrics` 노출
  - 파일: `tools/wb_worker/main.cpp`

## 패턴 개요
- 수집: 서버는 세션 이벤트를 Redis에 적재한다.
  - 권장: Redis Streams (`stream:session_events`) + 컨슈머 그룹
  - 대안: List(단순 큐). 내구성, 재처리, 확장성 면에서 Streams가 더 낫다.
- 처리: 워커가 배치 단위로 이벤트를 읽어 Postgres에 트랜잭션으로 반영한다.
- 커밋 전략: 시간 기반(max delay) + 개수/바이트 기반 임계치 동시 적용
- 내구성: Redis AOF everysec + Streams 보존 정책으로 유실 위험 최소화

Dynaxis가 List 대신 Streams를 기본으로 택한 이유는 운영 현실 때문이다. 단순 큐는 보기에는 쉽지만, 재처리, pending 추적, consumer group 확장, reclaim 같은 운영 기능이 부족하다. 처음에는 단순해 보여도 장애가 한 번 나면 수습 비용이 훨씬 커진다. Streams는 초기 개념이 조금 더 복잡하지만, 장기 유지보수와 장애 복구 비용을 줄여 준다.

## 이벤트 모델
- 타입 예시:
  - `session_login`, `session_logout`, `presence_heartbeat`, `room_join`, `room_leave`, `message_ack`, `typing_start/stop`
- 공통 필드:
  - `event_id`(Redis XADD ID), `type`, `ts_ms`(epoch ms), `user_id?`, `session_id`, `room_id?`, `gateway_id?`, `trace_id?`, `correlation_id?`
  - `payload`는 원본 Stream field들을 그대로 포함한 `jsonb`로 적재한다(값은 문자열).
  - 세션 식별자(session_id)는 서버 측에서 세션별 UUID v4를 생성하여 사용한다(내부 숫자 세션 식별자와 분리됨)
- 멱등성 키: `event_id`(Redis XADD ID)를 RDB에 unique로 저장하고 `ON CONFLICT DO NOTHING`으로 중복 삽입을 무해화한다.

멱등성을 먼저 설계하는 이유는 write-behind가 본질적으로 at-least-once 계열 동작이기 때문이다. 재시도와 reclaim이 있는 시스템에서 "중복이 생길 수 있다"는 사실 자체는 정상이다. 진짜 문제는 그 중복이 영속화 단계에서 부작용을 일으키는 것이다. 따라서 Dynaxis는 중복 가능성을 없애려 하기보다, 중복 삽입을 무해하게 만드는 방향을 택한다.

## 배치 커밋
- 트랜잭션 범위: 동일 테이블/유형끼리 묶거나, 다중 테이블 시 단일 트랜잭션
- 정책:
  - `BATCH_MAX_EVENTS`(예: 100–500)
  - `BATCH_MAX_BYTES`(예: 512 KiB–4 MiB)
  - `BATCH_MAX_DELAY_MS`(예: 500–3000ms). 기본값 500ms로 사실상 준실시간 반영.
- 실패 처리: 전체 롤백 후 재시도(지수 백오프). 부분 실패는 레코드 단위로 분리 재시도 또는 DLQ로 이동

배치를 쓰는 이유는 단순히 DB round-trip 수를 줄이기 위해서만이 아니다. 시간, 개수, 바이트 임계치를 함께 두는 것은 서로 다른 실패 모드를 막기 위해서다.

- 이벤트 수만 보면 payload가 큰 배치에서 메모리 사용이 튈 수 있다.
- 바이트만 보면 작은 이벤트가 너무 오래 쌓일 수 있다.
- 시간만 보면 트래픽 급증 시 DB commit 횟수가 너무 많아질 수 있다.

세 기준을 함께 두면 지연과 효율을 동시에 조절하기 쉽다.

## 전달 보장
- 모드: at-least-once(현실적인 선택). 멱등 삽입으로 중복을 무해화한다.
- exactly-once는 비용과 복잡도가 크다. 필요하면 테이블별 고유 제약과 이벤트 오프셋 체크포인트로 근접 구현한다.

Dynaxis가 exactly-once를 기본 목표로 두지 않는 이유는 이 경로에서 가장 중요한 것이 "사용자 경로를 느리게 하지 않으면서도 유실을 줄이는 것"이기 때문이다. exactly-once는 매력적인 목표처럼 보이지만 구현 복잡도와 운영 비용이 크게 늘어난다. 현재 write-behind는 at-least-once와 멱등 저장으로 대부분의 운영 문제를 더 단순하게 해결한다.

## 장애/재시작
- 워커는 Streams 컨슈머 그룹의 펜딩(pending) 항목을 reclaim/재처리하여 손실을 방지한다.
  - Redis 6.2+에서는 `XAUTOCLAIM` 기반 reclaim을 사용한다. (`WB_RECLAIM_*`)
- 서버 재시작: 미커밋 이벤트는 Streams에 남아 재처리 대상이 됨
- Redis 장애: 폴백으로 동기 기록(write-through) 전환 또는 기능 축소(설정 플래그)

재시작과 장애를 별도 섹션으로 두는 이유는, write-behind는 정상 시 성능보다 비정상 시 복구가 더 중요하기 때문이다. 서버가 재시작해도 Stream에 이벤트가 남아 있어야 하고, 워커가 죽었다 살아나도 pending을 다시 주워야 한다. 이 경로가 약하면 평소에는 빨라 보여도 실제 운영에서는 신뢰할 수 없다.

### 종료/Drain 정책
- `server_app`: shutdown 시 readiness를 내리고 acceptor를 먼저 중지해 신규 연결을 차단한다. 이후 기존 연결을 drain 하며 `SERVER_DRAIN_TIMEOUT_MS` 내에 정리되지 않으면 남은 연결은 강제 종료 경로로 전환한다.
- `wb_worker`: shutdown 시 신규 `XREADGROUP` 수집을 중단하고, 버퍼에 이미 적재된 배치는 마지막 flush로 소진한다. DB 비가용 구간에서는 `WB_DB_RECONNECT_BASE_MS`~`WB_DB_RECONNECT_MAX_MS` 지수 백오프를 유지한다.
- 운영 검증: server는 `chat_shutdown_drain_*`, worker는 `wb_pending`, `wb_flush_*`, `wb_db_reconnect_backoff_ms_last`를 함께 확인한다.

## 데이터 모델(예)
- `session_events`(append-only) — 이벤트소싱/감사 목적
  - `id bigserial`, `event_id text unique`, `type text`, `ts timestamptz`, `user_id uuid`, `session_id uuid`, `room_id uuid`, `payload jsonb`
  - 인덱스: `user_id, ts`, `session_id, ts`, `type, ts`
- 또는 `aggregates` — 압축된 스냅샷(예: presence 카운트). 이벤트는 TTL 후 정리 가능

## 일관성 등급
- 클라이언트 관점: 최종적 일관성(eventual consistency). 쓰기 직후 읽기는 Redis 캐시(세션/프레즌스)로 충족한다.
- RDB는 약간 뒤늦게 반영(T+Δ). 감사/리포팅/복구용

## 구성 키
- server_app
  - `WRITE_BEHIND_ENABLED` (bool)
  - `REDIS_STREAM_KEY=session_events`
  - `REDIS_STREAM_MAXLEN`(approx trim), `REDIS_STREAM_APPROX`(0이면 exact trim)
- wb_worker
  - `WB_GROUP`, `WB_CONSUMER`
  - `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_DELAY_MS`
  - `WB_RETRY_MAX`, `WB_RETRY_BACKOFF_MS`(flush 재시도 예산/백오프)
  - `WB_DLQ_STREAM=session_events_dlq`(옵션)
  - `WB_DLQ_ON_ERROR=1`(에러 시 DLQ로 포워드; 0이면 비활성)
  - `WB_ACK_ON_ERROR=1`(에러 시에도 ACK; 0이면 PEL에 남겨 재시도 유도)
  - `WB_DB_RECONNECT_BASE_MS=500`, `WB_DB_RECONNECT_MAX_MS=30000`(DB 재연결 지수 백오프)
  - `WB_RECLAIM_ENABLED=1`, `WB_RECLAIM_INTERVAL_MS`, `WB_RECLAIM_MIN_IDLE_MS`, `WB_RECLAIM_COUNT`
- 공통
  - `DB_URI`, `REDIS_URI`
  - `METRICS_PORT`(설정 시 `/metrics` 노출; server_app / wb_worker 공용)
  - `RUNTIME_TRACING_ENABLED`, `RUNTIME_TRACING_SAMPLE_PERCENT`(설정 시 stream->DB 경로 상관키 추적)

## 스트림(Streams) 운영 키/그룹/필드 정리
- 생산 키: `session_events`(=`REDIS_STREAM_KEY`)
- 소비 키: `session_events`(동일 스트림, 컨슈머 그룹 기반)
- 그룹: `wb_group`(예시, 설정 키 `WB_GROUP`로 주입)
- 컨슈머: 워커 인스턴스 식별자(예: `host-1:pid`), 설정 키 `WB_CONSUMER`
- 필드 집합(권장 최소):
  - `type`(예: `session_login` 등), `ts_ms`, `user_id`, `session_id`, `room_id?`, `gateway_id?`, `trace_id?`, `correlation_id?`
  - 멱등 보강 시: `idempotency_key` 포함 고려
- 기타 운영 키:
  - `WRITE_BEHIND_ENABLED`: 기능 토글
  - `REDIS_STREAM_KEY`: 스트림 키 이름
  - `WB_GROUP`: 컨슈머 그룹명(예: `wb_group`)
- `WB_CONSUMER`: 소비자 이름 접두 또는 전체명
- `REDIS_STREAM_MAXLEN`: XADD 시 approx trim 임계

## 서버 생산자 경로(적용 위치)
- 게이트: `WRITE_BEHIND_ENABLED`가 true일 때만 Streams로 XADD 수행. 키는 `REDIS_STREAM_KEY`(기본 `session_events`).
- 트림: `REDIS_STREAM_MAXLEN`가 설정되어 있으면 `XADD MAXLEN ~ <count>` 적용.
- 이벤트 매핑(파일/함수 기준):
  - 로그인 성공 → `session_login`
    - 파일: `server/src/chat/handlers_login.cpp`
    - 타이밍: 유저 식별자(UID) 확정 및 상태 갱신 후
    - 필드 예: `type=session_login`, `ts_ms`, `user_id`, `session_id`, `room_id` + (옵션) `ip`
  - 룸 입장 → `room_join`
    - 파일: `server/src/chat/handlers_join.cpp`
    - 타이밍: `state_.cur_room`/`rooms` 갱신 직후
    - 필드 예: `type=room_join`, `ts_ms`, `user_id`, `session_id`, `room_id`
  - 룸 퇴장 → `room_leave`
    - 파일: `server/src/chat/handlers_leave.cpp`
    - 타이밍: 방 멤버셋에서 제거 직후(로비 이동 전/후 중 한 지점으로 표준화)
    - 필드 예: `type=room_leave`, `ts_ms`, `user_id`, `session_id`, `room_id`
- 세션 종료 → `session_close`
    - 필드 해석: 공통 필드는 `type`, `ts_ms`, `session_id`, `user_id`, `room_id`, `gateway_id`; 상황별로 `room`, `user_name` 등을 추가한다.
    - 파일: `server/src/chat/session_events.cpp`
    - 타이밍: 상태 정리 및 브로드캐스트 직후
    - 필드 예: `type=session_close`, `ts_ms`, `user_id?`, `session_id`, `room_id?`
- 향후 후보(필요 시 도입):
  - 채팅 송신 이벤트 요약 또는 ACK(`handlers_chat.cpp`) → 메시지 영속화와의 관계를 고려해 선택
  - 타이핑 시작/중지(`typing_start/stop`) → 저중요 이벤트로 write-behind 1단계 도입에 적합
- 멱등성: `idempotency_key`(예: `session_id + ts_ms + type`) 또는 Streams ID 기반 처리. DB 고유 제약으로 중복 차단.

## 현재 워커 동작(구현됨)
- 배치 파라미터 적용: `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_DELAY_MS`
- 멱등성: `event_id`(Streams ID) 고유 제약으로 중복 삽입 무해
- 배치 커밋: 1 배치 = 1 트랜잭션 + 엔트리별 savepoint(subtransaction)로 부분 실패 격리
- flush 재시도 예산: `WB_RETRY_MAX`/`WB_RETRY_BACKOFF_MS` 범위 내에서 즉시 재시도 후, 예산 소진 시 PEL reclaim 경로로 이관
- ACK: commit 성공 후 ACK(At-least-once). 처리 실패는 DLQ 포워드(옵션) 후 ACK 정책으로 정리한다.
- PEL reclaim: `WB_RECLAIM_*`가 활성화되면 주기적으로 `XAUTOCLAIM`으로 pending을 회수한다.
- 메트릭: `METRICS_PORT`를 설정하면 `/metrics`에 다음을 노출한다.
  - pending/flush: `wb_pending`, `wb_flush_*`
  - reclaim: `wb_reclaim_*`
  - ack: `wb_ack_*`
  - db/backoff/drop: `wb_db_unavailable_total`, `wb_db_reconnect_backoff_ms_last`, `wb_error_drop_total`
 - tracing: stream field의 `trace_id`/`correlation_id`가 존재하면 wb_worker DB insert span 로그에 동일 상관키를 연결한다.

## 모니터링
- 레이턴시 p50/p95, 배치 크기, 커밋율, 실패율, 재시도/펜딩 길이, DLQ 길이

## DLQ 재처리 운영

- `wb_worker`가 실패 이벤트를 DLQ로 보낼 때, `wb_dlq_replayer`가 후속 재처리를 담당한다.
- 핵심 설정:
  - `WB_DLQ_STREAM`
  - `WB_RETRY_MAX`
  - `WB_RETRY_BACKOFF_MS`
- 운영 흐름:
  1. DLQ에서 오래된 항목을 읽는다.
  2. 재시도 예산 이내면 재처리한다.
  3. 성공 시 정리하고, 실패 시 재시도 카운트를 증가시킨다.
  4. 예산을 초과한 항목은 dead stream 또는 명시적 운영자 검토 대상으로 넘긴다.
- 모니터링 포인트:
  - DLQ backlog 길이
  - 재처리 성공, 재시도, dead 카운터
  - 원인 로그(SQL, 네트워크, validation failure)
- 운영자는 DLQ 급증 시 DB 가용성, 최근 배포 변경, payload 손상 여부를 먼저 본다.

## 점진 도입 전략
- 1단계: write-through + cache-aside(기본)
- 2단계: 비핵심 이벤트부터 write-behind 적용(typing, presence, acks)
- 3단계: 중요 이벤트 확대, 멱등 보강/알람 확립
- 4단계: 필요 시 Streams를 통한 내구 브로드캐스트로 통합

## 트레이드오프
- 장점: RDB 부하 감소, 지연 개선, 스파이크 흡수
- 단점: 최종적 일관성, 운영 복잡도 증가, Redis 의존 강화. 따라서 장애 대비 폴백과 알람이 필수다.
