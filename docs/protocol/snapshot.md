# 룸 상태 스냅샷(Snapshot)

목표: 룸 입장 시 최근 메시지와 워터마크를 한 번에 전달하고, 중복/누락 없이 브로드캐스트 스트림으로 이어붙이도록 한다. 스냅샷·브로드캐스트 모두 이름이 아닌 UUID(`room_id`, `user_id`, `session_id`)를 사용한다.

## 전송 방식
- 기본 옵션은 단일 패킷(Option A)으로, `MSG_STATE_SNAPSHOT` 한 개에 모든 데이터를 담는다.
- 메시지가 매우 길어질 경우에 대비해 BEGIN/CHUNK/END 프레이밍(Option B)을 리저브해 두며, `count`가 상한을 초과하면 자동으로 CHUNK 모드로 전환한다.

## 페이로드(payload) 구조
- opcode: `MSG_STATE_SNAPSHOT` (`server/protocol/game_opcodes.json`, `docs/protocol/opcodes.md`)
- 모든 정수는 big-endian, 문자열은 `lp_utf8`(length-prefixed UTF-8)을 사용한다.
- 필드
  1. `room_id` (16B UUID)
  2. `wm` (`u64`): snapshot 생성 시점의 최신 message id (워터마크)
  3. `count` (`u16`): 아래 `messages[]` 개수, 기본 0~1000
  4. `messages[count]`:
     - `id` (`u64`): message id (오름차순)
     - `user_id` (UUID16)
     - `created_at_ms` (`i64`): UTC epoch milliseconds
     - `content` (`lp_utf8`)
     - `sender_sid` (`u32`, optional): self-echo 필터용
     - `flags` (`u16`): system/공지 여부 등

## 제한 및 권고
- 단일 패킷은 64KB 이하가 되도록 `count`와 `content` 길이를 제한한다. 기본 상한은 20개, 최대 1000개.
- `messages[]`는 `id` 오름차순으로 정렬하고, `wm` 이상이 되면 바로 브로드캐스트 스트림으로 넘긴다.

## 클라이언트 처리
- 클라이언트는 `max_id`를 추적하고, 스냅샷을 적용할 때 `max_id = max(wm, messages.last.id)`로 업데이트한다.
- 스냅샷 적용이 끝날 때까지 입력창을 비활성화해 중복 전송을 막는다. 브로드캐스트 스트림에서 `id <= max_id`는 중복으로 간주한다.

## 오류 및 폴백
- 스냅샷 생성 실패는 `MSG_ERR(SNAPSHOT_TEMPORARY|SNAPSHOT_FATAL)`로 구분한다. 일시 오류는 재시도하고, 권한 문제 등 치명적 오류는 UI에 표시한다.
- Redis 캐시가 비어 있을 경우 Postgres에서 최근 N개를 조회해 채운다(`server/src/chat/chat_service_core.cpp:282`). 조회 실패 시 빈 스냅샷을 전송하고 브로드캐스트만 이어붙인다.
