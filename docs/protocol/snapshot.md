# 룸 상태 스냅샷(Snapshot)

이 문서는 현재 room snapshot contract의 source of truth다.
목표는 룸 입장 시 최근 메시지와 워터마크를 한 번에 전달하고, 이어지는 브로드캐스트 스트림과 중복/누락 없이 연결하는 것이다.

## 목적

- join 직후 최근 메시지를 빠르게 제공한다
- snapshot 이후 fanout과 자연스럽게 이어 붙인다
- Redis miss 또는 장애 시 Postgres 폴백 경로를 명시적으로 유지한다
- canonical 식별자는 이름이 아니라 UUID(`room_id`, `user_id`, `session_id`)다

## Join -> Snapshot -> Fanout Flow

1. client가 `JOIN_ROOM(room_id)`를 보낸다.
2. server가 recent-history cache와 DB fallback을 사용해 snapshot payload를 준비한다.
3. server가 `MSG_STATE_SNAPSHOT{ room_id, wm, messages[] }`를 전송한다.
4. client는 snapshot을 렌더링하고 `max_id = max(wm, messages.last.id)`로 갱신한다.
5. 이후 fanout `MSG_CHAT_BROADCAST`는 `id <= max_id`면 중복으로 버리고, 그보다 큰 메시지만 새 이벤트로 처리한다.

## Payload Contract

- opcode: `MSG_STATE_SNAPSHOT`
- 주요 필드:
  - `room_id`
  - `wm` (`u64`): snapshot 생성 시점의 최신 message id
  - `count`
  - `messages[count]`
- 각 메시지는 오름차순 `id`로 정렬한다.
- 모든 정수는 big-endian, 문자열은 `lp_utf8`를 사용한다.

## Recent-History Cache Behavior

### Redis keys

- `room:{room_id}:recent`
  - 최신 `message_id` 목록을 보관하는 LIST
- `msg:{message_id}`
  - snapshot 직렬화 payload를 보관하는 개별 cache entry

### Join-time read path

- server는 먼저 `LRANGE room:{room_id}:recent ...`로 recent message ids를 읽는다.
- 각 `message_id`에 대해 `GET msg:{id}`를 시도하고, 살아 있는 entry만 snapshot candidate로 사용한다.
- cache miss 또는 partial miss일 때만 Postgres에서 최근 N개를 보충 조회한다.
- DB에서 보충한 메시지는 즉시 cache에 write-through 한다.

### Chat-time write path

- 메시지 영속 성공 후 `cache_recent_message()`가:
  - `SETEX msg:{id}`
  - `LPUSH/LTRIM room:{room_id}:recent`
  를 수행한다.

## Watermark And Client Rules

- `wm`은 snapshot 생성 시점의 최신 `message_id`다.
- client는 `max_id = max(wm, messages.last.id)`를 유지한다.
- fanout에서 `id <= max_id`는 중복으로 버린다.
- snapshot 적용 전까지 입력 UI를 잠시 비활성화해 중복 전송/중복 렌더 위험을 줄인다.

## Errors And Fallback

- snapshot 생성 실패는 `MSG_ERR(SNAPSHOT_TEMPORARY|SNAPSHOT_FATAL)`로 구분한다.
- Redis 장애/파서 오류는 즉시 DB 경로로 폴백한다.
- DB 조회까지 실패하면 빈 snapshot을 전송하고, 이후 fanout만 이어 붙인다.
- Postgres 장애 시 cache에 남아 있는 recent-history만 전송할 수 있다.

## Related Docs

- `docs/protocol.md`
- `docs/protocol/opcodes.md`
- `docs/db/write-behind.md`
