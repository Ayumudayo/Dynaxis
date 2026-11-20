# 방 snapshot과 실시간 메시지 병합

`MSG_STATE_SNAPSHOT`(0x0200)은 `proto/wire.proto`의 Protobuf `StateSnapshot`이다. 문자열 lp_utf8와 고정 count를 직접 나열하는 별도 snapshot 인코딩은 사용하지 않는다.

## Wire 계약

- `current_room`: 표시용 방 이름, `room_id`: 영속 방 세대의 UUID.
- `rooms`, `users`: 현재 표시 상태.
- `messages`: 영속 `id`, sender, text, ts_ms를 가진 최근 메시지. ID 오름차순이다.
- `watermark`: snapshot의 DB 조회 경계에서 확인한 최신 commit ID.
- `last_seen_id`: membership의 읽음 위치. watermark로 대체하지 않는다.
- `history_available`: DB 기준 최근 창을 읽었으면 true. false이면 캐시의 부분 이력이거나 이력 조회 실패다.
- `history_limit`: 최근 창의 최대 크기. 기본 20개이며 전체 기간 replay가 아니다.

`MSG_CHAT_BROADCAST`(0x0101)도 Protobuf이며 `message_id`와 `room_id`를 함께 전달한다. 영속 메시지의 ID는 commit 확인 뒤 부여하고, 시스템 알림은 `message_id=0`이다. 필드는 기존 번호를 유지하며 추가했다.

## 서버 경계

writer는 방별 DB 잠금을 ID 할당 전부터 commit까지 유지한다. snapshot도 같은 잠금에서 watermark와 최근 창을 읽으므로, 진행 중인 작은 ID의 writer를 건너뛰어 watermark를 확정하지 않는다. 최근 창은 해당 방의 최신 N개를 선택한 뒤 ID 오름차순으로 반환한다. 다른 방의 sequence 간격 때문에 임의로 `max_id-N`부터 잘라 읽지 않는다.

DB 조회 실패 시 Redis recent cache를 사용할 수 있지만 `history_available=false`, `watermark=0`으로 완전한 경계와 구분한다. 캐시는 commit이 확인된 메시지만 갱신한다.

## 클라이언트 규칙

1. snapshot과 live 메시지를 `(room_id, message_id)`로 병합한다.
2. 이미 보관한 같은 ID만 제거한다. watermark보다 작은 ID라는 이유로 늦게 도착한 live 메시지를 버리지 않는다.
3. 같은 방의 영속 메시지는 ID 순서로 표시한다. snapshot 전후 어떤 것이 먼저 도착해도 같은 규칙을 사용한다.
4. 같은 방 이름의 `room_id`가 바뀌면 이전 세대의 화면 이력을 제거한다.
5. 이력 조회가 불완전하면 사용자에게 표시한다. 오래된 메시지가 최근 창 밖에 있으면 현재 snapshot으로 복원할 수 없다.

개발 클라이언트는 최대 2000개 표시 항목을 보관한다. 무제한 중복 ID 저장소나 영구 로컬 history는 제공하지 않는다. 서버 전달의 실패·재시도 범위는 [기초 계약](../architecture/foundation-contracts.md)에 있다.
