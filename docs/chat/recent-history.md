# 입장 시 최근 룸 기록 (Redis 기반)

최근 접속자가 방에 합류할 때 DB를 전부 다시 읽지 않고 Redis 캐시를 우선 활용하도록 한 설계 문서이다. 목적은 ① 마지막 대화 20건 정도를 즉시 보여 주고, ② 이미 읽은 영역과 새 메시지 영역을 명확히 구분하며, ③ Redis 장애 시에도 Postgres 로우를 안전하게 폴백하는 것이다.

## 핵심 구성 요소
- **SoR**: Postgres `messages` 테이블 (`id` = bigserial, `created_at_ms` 컬럼 사용)
- **Redis 키 공간**
  - `room:{room_id}:recent` : LIST, 가장 최신 메시지 id를 보관. 길이는 `ROOM_RECENT_MAXLEN` 까지 유지.
  - `msg:{message_id}` : Protobuf 직렬화 바이트( `StateSnapshot::SnapshotMessage` ), `CACHE_TTL_RECENT_MSGS` 만큼 유지.
- **환경 변수**
  - `RECENT_HISTORY_LIMIT` (기본 20) : 클라이언트에 최종적으로 보여 줄 메시지 수
  - `RECENT_HISTORY_FETCH_FACTOR` (기본 3) : DB 재조회 시 limit × factor 만큼 버퍼 확보
  - `ROOM_RECENT_MAXLEN` (기본 200) : Redis LIST 최대 길이
  - `CACHE_TTL_RECENT_MSGS` (기본 6시간) : 메시지 payload TTL

## 입장(Join) 시 플로우 (`chat_service_core.cpp::send_snapshot`)
1. **룸 ID 확보** : 메모리 캐시(`state_.room_ids`) 확인 후 없으면 `ensure_room_id_ci()`로 생성/조회.
2. **Redis 조회** :
   - `LRANGE room:{rid}:recent -RECENT_HISTORY_LIMIT -1` 호출.
   - 각 id에 대해 `GET msg:{id}`를 시도하고 성공한 것만 `StateSnapshot::SnapshotMessage`로 변환.
   - 일부가 만료되어도 살아 있는 항목만 유지(부분 HIT 허용).
3. **DB 폴백** :
   - Redis MISS 또는 캐시가 비었을 때만 Postgres에서 `fetch_recent_by_room(rid, since_id, fetch_count)` 실행.
   - `since_id` 계산 시 마지막 읽은 메시지(`memberships.last_seen`)를 반영해 이미 읽은 영역과 gap 컨텍스트를 맞춘다.
   - DB에서 가져온 메시지는 곧바로 `cache_recent_message()`를 호출해 Redis에 write-through.
4. **응답 구성** :
   - `StateSnapshot`에 `rooms`, `users`, `messages`, `last_seen_id`를 채워 `MSG_STATE_SNAPSHOT`으로 전송한다.

## 메시지 저장 시 플로우 (`handlers_chat.cpp::on_chat_send`)
1. 메시지를 Postgres에 영속 (`messages().create`).
2. 영속에 성공하면 `StateSnapshot::SnapshotMessage` 형태로 직렬화 후 `cache_recent_message()` 호출:
   - `SETEX msg:{id}` 로 payload 저장.
   - `LPUSH/LTRIM room:{rid}:recent` 로 id 목록 갱신.
3. 실패 시 WARN 로그를 남기고 다음 요청이 DB에서 재구성하도록 한다.

## 캐시 일관성 & 장애 대응
- LIST와 payload 중 하나라도 TTL 만료/삭제되면 부분 HIT 로그를 남기고 부족한 부분만 DB에서 보충한다.
- Redis 연결 실패, 파서 오류 등은 WARN 로그와 함께 즉시 DB 경로로 돌아간다.
- Postgres 조회가 실패하면 snapshot은 빈 메시지 목록으로라도 전송되며, `last_seen_id`는 0으로 남는다.

## 사용자 인터페이스(UI) 가이드
- `snapshot_complete` 이벤트를 받을 때 `last_seen_id`를 저장해 이후 `/refresh` 나 poll 시 기준으로 사용한다.
- `id <= last_seen_id` 인 메시지는 “이미 읽음” 상태로 처리하고, 그보다 큰 id만 신규 메시지로 강조한다.
- `/refresh` 명령은 서버 측에서 snapshot을 다시 보내므로 프론트엔드는 지연이 발생할 수 있음을 사용자에게 안내한다.

## 운영 체크리스트
- `RECENT_HISTORY_LIMIT`, `ROOM_RECENT_MAXLEN`, `CACHE_TTL_RECENT_MSGS` 값을 트래픽에 맞춰 조정한다. (권장: LIMIT=20~50, MAXLEN=5×LIMIT 이상)
- Redis 용량이 부족할 경우 TTL을 줄이거나 LIST 대신 ZSET으로 교체하는 대안을 고려한다.
- 캐시 miss 비율이 급격히 증가하면 `chat_service_core` WARN 로그와 Redis 메모리 상태를 점검한다.

## 향후 개선 사항
1. LIST 대신 ZSET + trimming 을 적용해 멀티 인스턴스 환경에서 순서를 더 명확히 보장.
2. `memberships.last_seen` 정보를 Redis에도 캐싱해 DB 읽기를 줄이는 방안 검토.
3. 캐시 HIT/폴백 경로에 대한 GTest 케이스 추가(채널 join 시 Redis stub을 활용해 검증). `tests/` 백로그에 추가.
