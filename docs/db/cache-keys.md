# Redis Key Schema and TTLs

일관된 키 네임스페이스와 TTL 정책으로 운영 가시성과 충돌을 방지한다.

## 네임스페이스 규칙
- 접두사: `chat:{env}:`
- 도메인: `session|presence|room|msg|rate|idem`
- 리소스 식별자: UUID/해시/숫자 id

## 키 설계 예시
- 세션: `chat:{env}:session:{token_hash}` → value: `user_id` (string), TTL=`CACHE_TTL_SESSION`
- 프레즌스(유저): `chat:{env}:presence:user:{user_id}` → value: 1, TTL=heartbeat 주기 + 그레이스
- 프레즌스(룸): `chat:{env}:presence:room:{room_id}` → SET of `user_id`
- 룸 멤버: `chat:{env}:room:{room_id}:members` → SET, TTL=옵션(변경 시 갱신)
- 최근 메시지 id: `chat:{env}:room:{room_id}:recent` → LIST/ZSET of `message_id`
- 메시지 본문: `chat:{env}:msg:{message_id}` → JSON, TTL=`CACHE_TTL_RECENT_MSGS`
- 레이트: `chat:{env}:rate:{user_id}:{bucket}` → counters with TTL
- 아이도템포턴시: `chat:{env}:idem:{client_id}:{nonce}` → TTL

## TTL 가이드라인(권장 기본값)
- `CACHE_TTL_SESSION`: 만료 시간과 동일(예: 24h~7d)
- `CACHE_TTL_RECENT_MSGS`: 기본 2h(범위 1h~6h)
- `CACHE_TTL_MEMBERS`: 기본 10m(범위 5m~30m, 또는 무제한 + 변경 트리거 갱신)

## 메모리 예산 가이드(대략치)
- 메시지 본문 키 `msg:{id}`(JSON): 평균 300B 콘텐츠 기준 ≈ 500–800B/건
- 최근 목록 `room:{room_id}:recent`(LIST): 포인터/오버헤드 포함 ≈ 50–100B/항목
- 예산 산식(대략): `rooms * recent_len * (list_entry + msg_entry)`
  - 예) 1,000개 룸, recent_len=200 → 약 150–180MB 수준(헤드룸 30% 포함)
- 운영 시 `maxmemory`와 `allkeys-lru`/`volatile-lru` 정책을 함께 고려한다.

## 직렬화
- 값은 JSON 문자열 권장(디버깅 용이). 고성능이 필요하면 MessagePack로 변경 가능
