# Redis Strategy: Cache, Sessions, Fanout

목표: RDB(PostgreSQL) I/O를 줄이고, 핫패스 지연을 낮추며, 멀티 인스턴스 팬아웃을 효율화한다. Redis는 보조 계층이며, 영속성의 소스 오브 트루스는 Postgres다.

## 언제 Redis를 쓰는가
- 세션/프레즌스와 같은 휘발 데이터
- 최근 데이터 캐시(방 최근 메시지, 룸 멤버십)
- 레이트 리미팅/아이도템포턴시 키(idempotency)
- 팬아웃: Pub/Sub 또는 Streams를 통한 멀티 인스턴스 브로드캐스트

## 사용 패턴
- Cache-aside: 캐시 미스 시 Postgres 조회 → Redis 채움 → 응답
- Write-through(선택): Postgres 커밋 성공 후 Redis 갱신
- Write-behind는 초기에 미도입(일관성/복잡도 고려)

## 유스케이스 설계
1) 세션(Session Store)
- Redis: `SETEX session:{token_hash} user_id ttl`
- Postgres: 감사를 위해 세션 레코드 유지(토큰 해시, 만료, 철회 시간)
- 흐름: 로그인 성공 → Postgres INSERT → Redis SETEX → 응답

2) 프레즌스(Presence)
- 키: `presence:room:{room_id}` (SET) / `presence:user:{user_id}` (key with TTL)
- 입장 시 SADD, 퇴장/타임아웃 시 SREM 또는 TTL 만료
- 소스 오브 트루스는 Redis(휘발), 통계/감사는 필요 시 Postgres 이벤트 테이블로 적재

3) 룸 멤버십 캐시
- 키: `room:{room_id}:members` (SET of user_id), TTL 혹은 만료 없음 + 변경 시 갱신
- 소스 오브 트루스: Postgres; 변경 트랜잭션 커밋 후 Redis 갱신(write-through)
- 미스 시 Postgres에서 로드하여 캐시 채움

4) 메시지 팬아웃
- Pub/Sub: `fanout:room:{room_name}` 채널. 저지연, 비내구. 서버 인스턴스가 구독 → 세션에 브로드캐스트
- Streams: `stream:room:{room_id}`. 내구 + 컨슈머 그룹으로 백프레셔/재처리. 보관 기간/길이 정책 설정(trim)
- 권장: 초기에는 Postgres에 메시지 영속화 + Redis Pub/Sub로 브로드캐스트. 재전송/복구가 중요해지면 Streams로 전환

5) 최근 메시지 캐시
- 키: `room:{room_id}:recent` (LIST or ZSET of message_id), `msg:{id}` (HASH/JSON)
- 정책: 최근 N개만 유지(예: `ROOM_RECENT_MAXLEN=200`), TTL(예: 1–6시간). 미스 시 Postgres 페이징
- 조인 시 로딩: 기본 20개(`RECENT_HISTORY_LIMIT=20`), 워터마크와 오름차순 정렬로 순서 안정화. 세부는 `docs/chat/recent-history.md` 참조

6) 레이트 리미팅/아이도템포턴시
- 토큰 버킷: Lua 스크립트로 `rate:{user_id}` 카운터/윈도우
- 아이도템포턴시: `idem:{client_id}:{nonce}`를 TTL로 저장하여 중복 처리 방지

## 내구성/가용성
- Redis는 보조 캐시/큐로 사용. 데이터 유실 허용 범위 내에서 설계(Pub/Sub는 유실 가능, Streams는 보존)
- 설정: AOF everysec + RDB 스냅샷, 레플리카 구성 고려. 클러스터는 키 설계와 멀티키 연산 제약을 인지

## 장애/폴백
- Redis 장애 시: 캐시 미스 증가 → Postgres로 폴백, 기능 지속. 팬아웃은 일시적으로 서버 내부 브로드캐스트로 축소
- 과부하 시: TTL 단축/캐시 범위 축소, Pub/Sub 대신 Streams로 백프레셔 적용 검토

## 설정 키 제안
- `REDIS_URI` (예: `redis://localhost:6379/0`)
- `REDIS_POOL_MAX` (연결 풀 최대치)
- `REDIS_CHANNEL_PREFIX` (예: `chat:`)
- `REDIS_USE_STREAMS` (bool), `REDIS_STREAM_MAXLEN` (approx trim)
- `CACHE_TTL_SESSION`, `CACHE_TTL_RECENT_MSGS`, `CACHE_TTL_MEMBERS`

## 키 스키마 가이드
- 네임스페이스: `chat:{env}:{domain}:{id}` 형태 권장
- 키는 반드시 UUID 기반 식별자를 사용하며, 이름 기반 키를 금지한다
- 예: `chat:dev:session:{token_hash}`, `chat:dev:room:{room_id}:members`, `chat:dev:presence:room:{room_id}`

## 선택 요약
- 소스 오브 트루스: Postgres
- 성능/지연 최적화: Redis (세션/프레즌스/캐시/팬아웃)
- 메시지 영속: Postgres, 브로드캐스트: Redis Pub/Sub → 필요 시 Streams

## Presence/PubSub 업데이트
- User Presence: `presence:user:{user_id}` 키에 1을 SETEX로 저장하여 TTL로 온라인 상태 유지(기본 `PRESENCE_TTL_SEC=30`). 로그인/채팅/핑(PING) 시 갱신한다.
- Room Presence: 입장 시 SADD `presence:room:{room_id}` `{user_id}`, 퇴장/세션 종료 시 SREM 처리한다.
- Pub/Sub 채널: `fanout:room:{room_name}`. `USE_REDIS_PUBSUB!=0`이면 Protobuf ChatBroadcast payload를 publish하며 Envelope(`gw=<gateway_id>\n<payload>`) 규칙을 따른다.
- self-echo 필터: 구독 측은 로컬 `GATEWAY_ID`와 비교해 동일한 gateway에서 온 메시지를 드롭한다.
- `GATEWAY_ID`를 설정하지 않으면 기본값 `gw-default`가 적용되므로, 멀티 게이트웨이 환경에서는 고유 값을 지정한다.

### 구독 안정성/운영
- 구독 루프는 예외 발생 시 점증 백오프로 재연결/재구독한다(최대 1s).
- 프로세스 종료 신호 수신 시 구독을 먼저 중단(stop_psubscribe)하여 중복/유실을 최소화한다.

## 운영/설정 키
- `PRESENCE_TTL_SEC` (기본 30): `presence:user:{user_id}` TTL
- `USE_REDIS_PUBSUB` (기본 0): 0이 아니면 Pub/Sub 발행 활성화
- `PRESENCE_CLEAN_ON_START` (기본 0): 부팅 시 `presence:room:*` 정리(개발/단일 인스턴스 사용 권장)
- `GATEWAY_ID`: Pub/Sub Envelope에 삽입할 Gateway 식별자, 미설정 시 `gw-default`.
