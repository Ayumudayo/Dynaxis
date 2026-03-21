# 룸 상태 스냅샷

이 문서는 현재 room snapshot 계약의 단일 기준 문서다. 목표는 룸 입장 시 최근 메시지와 워터마크를 한 번에 전달하고, 이어지는 브로드캐스트 스트림과 중복이나 누락 없이 자연스럽게 이어 붙이는 것이다.

이 문서가 중요한 이유는 입장 직후의 사용자 경험이 "이전 대화가 보이는가"와 "그 직후 새 메시지가 끊김 없이 붙는가"에 동시에 달려 있기 때문이다. 스냅샷과 팬아웃 연결 규칙이 느슨하면, 사용자는 같은 메시지를 두 번 보거나, 반대로 중요한 최근 메시지를 놓치게 된다.

## 목적

- join 직후 최근 메시지를 빠르게 제공한다.
- snapshot 이후 fanout과 자연스럽게 이어 붙인다.
- Redis miss나 장애 시 Postgres fallback 경로를 명시적으로 유지한다.
- 기준 식별자는 이름이 아니라 UUID(`room_id`, `user_id`, `session_id`)다.

이 목적을 따로 적는 이유는 snapshot이 단순 캐시 기능이 아니기 때문이다. 이는 사용자 초기 상태 복원과 이후 실시간 흐름 연결을 동시에 책임지는 계약이다.

## Join -> Snapshot -> Fanout 흐름

1. client가 `JOIN_ROOM(room_id)`를 보낸다.
2. server가 recent-history cache와 DB fallback을 사용해 snapshot payload를 준비한다.
3. server가 `MSG_STATE_SNAPSHOT{ room_id, wm, messages[] }`를 전송한다.
4. client는 snapshot을 렌더링하고 `max_id = max(wm, messages.last.id)`로 갱신한다.
5. 이후 fanout `MSG_CHAT_BROADCAST`는 `id <= max_id`면 중복으로 버리고, 그보다 큰 메시지만 새 이벤트로 처리한다.

이 흐름을 이렇게 고정하는 이유는 snapshot과 fanout 사이에 "경계 값"이 반드시 있어야 하기 때문이다. 그 경계가 바로 `wm`과 `max_id`다. 이 값이 없으면 snapshot 직후 들어온 메시지를 중복으로 그리거나 놓치기 쉽다.

## Payload 계약

- opcode: `MSG_STATE_SNAPSHOT`
- 주요 필드:
  - `room_id`
  - `wm` (`u64`): snapshot 생성 시점의 최신 `message_id`
  - `count`
  - `messages[count]`
- 각 메시지는 오름차순 `id`로 정렬한다.
- 모든 정수는 big-endian, 문자열은 `lp_utf8`를 사용한다.

오름차순 정렬과 고정 인코딩을 강제하는 이유는 client가 단순한 규칙으로만 복원하도록 하기 위해서다. snapshot 해석 규칙이 복잡해질수록 클라이언트별 구현 차이가 생기고, 같은 서버 응답이 플랫폼마다 다르게 보일 수 있다.

## 최근 이력 캐시 동작

### Redis 키

- `room:{room_id}:recent`
  - 최신 `message_id` 목록을 보관하는 LIST
- `msg:{message_id}`
  - snapshot 직렬화 payload를 보관하는 개별 cache entry

### join 시 읽기 경로

- server는 먼저 `LRANGE room:{room_id}:recent ...`로 최근 `message_id`를 읽는다.
- 각 `message_id`에 대해 `GET msg:{id}`를 시도하고, 살아 있는 entry만 snapshot 후보로 사용한다.
- cache miss 또는 partial miss일 때만 Postgres에서 최근 N개를 보충 조회한다.
- DB에서 보충한 메시지는 즉시 cache에 write-through 한다.

이 읽기 순서가 중요한 이유는 Redis와 DB를 동시에 두는 비용을 그냥 감수하기 위해서가 아니다. 평소에는 Redis로 빠르게 응답하고, miss가 생겼을 때만 DB로 내려가서 복원하는 것이 지연과 복구 가능성 사이의 균형이 가장 좋기 때문이다.

### 채팅 시 쓰기 경로

- 메시지 영속 성공 뒤 `cache_recent_message()`가 아래를 수행한다.
  - `SETEX msg:{id}`
  - `LPUSH/LTRIM room:{room_id}:recent`

write-through를 유지하는 이유는 snapshot cache가 "대충 맞는 힌트"가 아니라 입장 시 바로 사용자에게 보이는 데이터이기 때문이다. 영속화와 캐시 갱신을 너무 느슨하게 분리하면, 막 쓴 메시지가 입장 직후 snapshot에는 빠지는 일이 잦아진다.

## 워터마크와 클라이언트 규칙

- `wm`은 snapshot 생성 시점의 최신 `message_id`다.
- client는 `max_id = max(wm, messages.last.id)`를 유지한다.
- fanout에서 `id <= max_id`는 중복으로 버린다.
- snapshot 적용 전까지 입력 UI를 잠시 비활성화해 중복 전송과 중복 렌더 위험을 줄인다.

클라이언트 규칙을 서버 문서에 같이 적는 이유는 이 계약이 양방향 계약이기 때문이다. 서버가 `wm`을 보내는 것만으로는 충분하지 않고, 클라이언트가 같은 의미로 해석해야 중복과 누락이 동시에 줄어든다.

## 오류와 fallback

- snapshot 생성 실패는 `MSG_ERR(SNAPSHOT_TEMPORARY|SNAPSHOT_FATAL)`로 구분한다.
- Redis 장애나 파서 오류는 즉시 DB 경로로 fallback 한다.
- DB 조회까지 실패하면 빈 snapshot을 전송하고, 이후 fanout만 이어 붙인다.
- Postgres 장애 시 cache에 남아 있는 recent-history만 전송할 수 있다.

이 fallback 계층을 명시하는 이유는 장애 시에도 "어디까지는 보장하는가"를 분명히 하기 위해서다. 완전한 snapshot이 불가능한 상황에서도 fanout을 계속 이어 붙일 수 있으면, 사용자는 적어도 이후 새 메시지는 놓치지 않는다.

## 관련 문서

- `docs/protocol.md`
- `docs/protocol/opcodes.md`
- `docs/db/write-behind.md`
