# 방 스냅샷 프로토콜(최종안)

목표: 방 입장 시 최근 메시지 스냅샷을 순서 보장/중복 방지와 함께 전달한다. 스냅샷·브로드캐스트 모두 이름이 아닌 UUID(`room_id`, `user_id`, `session_id`)를 사용한다.

## 최종 선택
- Option A(단일 스냅샷 메시지)를 기본으로 채택한다. 스냅샷이 커질 경우를 대비해 Option B(BEGIN/END/CHUNK)는 리저브로 남겨둔다.

## 메시지 및 바이너리 포맷
- 사용 opcode: `MSG_STATE_SNAPSHOT`
- 수치형은 big-endian, 문자열은 `lp_utf8`(length-prefixed UTF-8)을 사용한다. 관련 유틸: `protocol/frame.hpp`의 `read/write_be*`, `lp_utf8`.

Payload 구조(순서 고정):
1) `room_id` — 16바이트 UUID(네이티브 바이트 순서)
2) `wm` — `u64`(워터마크: 스냅샷 준비 시점의 최신 message id)
3) `count` — `u16`(포함 메시지 개수, 최대 1000 권장)
4) `messages[]` — `count` 회 반복
   - `id` — `u64`
   - `user_id` — 16바이트 UUID
   - `created_at_ms` — `i64`(UTC epoch millis)
   - `content` — `lp_utf8`

제한/권고:
- 한 프레임 최대 크기 내에서 `count`를 조절한다(초기값: 20개).
- 메시지는 id 오름차순으로 정렬해 전송한다.

## 정렬/중복 규칙
- 클라이언트는 `max_id`를 추적한다.
- 스냅샷 이후 도착한 방송 메시지에서 `id <= wm`은 드랍(멱등).
- UI는 스냅샷 적용 완료 전까지 입력/전송 비활성화.

## 에러 처리/폴백
- 스냅샷 실패: 재시도 가능(일시 장애)과 치명적(권한/존재) 에러 코드 구분.
- Redis 미스/장애 시 서버는 Postgres에서 조회 후 동일 포맷으로 전송한다.
