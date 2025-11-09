# Redis Strategy: Cache, Sessions, Fanout

목표: PostgreSQL을 SoR로 유지하면서 Redis를 세션/프레즌스/브로드캐스트/레이트리밋 캐시로 사용해 대기시간을 줄이고 백오프 전략을 단순화한다. Redis 장애 시에는 Postgres 경로로 폴백해도 무방하도록 write-through/write-behind 구성을 병행한다.

## 핵심 역할
- **세션/토큰 저장소**: Postgres는 장기 보관, Redis는 `session:{token_hash}` 키에 TTL을 둔 캐시.
- **Presence/멤버십 캐시**: 실시간 상태는 Redis SET/TTL을 사용하고, 만료/재시작 시 Postgres 이벤트로 재동기화.
- **브로드캐스트 팬아웃**: Redis Pub/Sub은 저지연 팬아웃, Streams는 내구성과 재처리를 담당.
- **최근 메시지**: 룸 입장 시 Redis 캐시(LIST/JSON)에서 로딩, 누락 시 Postgres 보충.
- **레이트리밋/멱등성**: `rate:{user_id}:{bucket}`, `idem:{client_id}:{nonce}` 키를 활용해 브루트포스·중복을 방지.

## 구성 요소 별 상세
### 1) 세션(Session Store)
- Redis: `SETEX session:{token_hash} user_id ttl`로 토큰↔사용자를 매핑한다. TTL은 웹 세션 요구사항에 맞게 1~24h.
- Postgres: 세션 테이블은 생성/만료 기록을 보존하고, 감사·로그아웃 시 authoritative 하게 사용한다.
- 흐름: 로그인 성공 → Postgres INSERT → Redis SETEX. 로그아웃/만료 → Redis DEL + Postgres 업데이트.

### 2) Presence
- 키: `presence:user:{user_id}` (SETEX), `presence:room:{room_id}` (SET of user_id).
- 로그인/heartbeat 시 `SETEX presence:user`와 `SADD presence:room`을 수행하고, 퇴장/세션종료 시 `SREM`/`DEL`을 호출한다.
- `PRESENCE_CLEAN_ON_START=1`이면 단일 인스턴스 개발 환경에서만 사용해 재시작 시 `presence:room:*`를 정리한다. 다중 인스턴스에서는 비활성화.

### 3) 룸 멤버십 캐시
- 키: `room:{room_id}:members` (SET). TTL을 두거나, 쓰기 시점에 항상 `SADD/SREM`을 사용해 write-through.
- Postgres `memberships` 테이블을 authoritative 로 사용하되, Redis 캐시는 `/who` 응답과 UI 표시를 빠르게 하기 위한 용도.

### 4) 팬아웃
- Pub/Sub: `fanout:room:{room_name}` 채널에 `gw=<id>\n<payload>` 형식으로 publish. 다중 Gateway 환경에서 self-echo를 막기 위해 `GATEWAY_ID`를 반드시 지정.
- Streams: `stream:room:{room_id}`를 사용해 내구성과 backlog 재처리를 지원한다. `REDIS_USE_STREAMS=1`, `REDIS_STREAM_MAXLEN`으로 approximate trimming 적용.

### 5) 최근 메시지 캐시
- 키: `room:{room_id}:recent`, `msg:{message_id}`. 상세는 `docs/chat/recent-history.md` 참고.
- 설정: `RECENT_HISTORY_LIMIT` 기본 20, `ROOM_RECENT_MAXLEN` 200, `CACHE_TTL_RECENT_MSGS` 6h.

### 6) 레이트리밋 / 멱등성
- 레이트리밋: `rate:{scope}:{bucket}`(예: `rate:user:{user_id}:5s`) 키에 counter + TTL을 둔다.
- 멱등성: `idem:{client_id}:{nonce}`를 SETEX로 기록해 중복 요청을 무시한다.

## 운영/환경 변수
| 변수 | 설명 |
| --- | --- |
| `REDIS_URI` | `redis://user:pass@host:6379/db` 형식. 미지정 시 Redis 기능 비활성화 |
| `REDIS_POOL_MAX` | 클라이언트 풀 최대 연결 수 |
| `REDIS_CHANNEL_PREFIX` | Pub/Sub prefix. 예: `knights:` |
| `REDIS_USE_STREAMS`, `REDIS_STREAM_MAXLEN` | Streams 사용 여부와 maxlen(approx) |
| `CACHE_TTL_SESSION`, `CACHE_TTL_RECENT_MSGS`, `CACHE_TTL_MEMBERS` | 각 캐시 TTL(초) |
| `PRESENCE_TTL_SEC`, `PRESENCE_CLEAN_ON_START` | Presence TTL 및 재시작 클린업 옵션 |
| `USE_REDIS_PUBSUB` | 1이면 브로드캐스트를 Redis로 보내고, 0이면 단일 서버 로컬 브로드캐스트 |
| `WRITE_BEHIND_ENABLED` | Streams 기반 write-behind 활성화 여부 |
| `GATEWAY_ID` | Pub/Sub Envelope용 gateway 식별자 |

## 키 이름 규칙
- 기본 패턴: `chat:{env}:{domain}:{id}`.
- 예: `chat:dev:session:{token_hash}`, `chat:prod:room:{room_id}:members`, `chat:prod:presence:user:{user_id}`.
- UUID 기반 ID를 사용하고, 문자열(닉네임 등)을 키에 직접 쓰지 않는다.

## 운영 체크리스트
- Redis ping p95 > 20ms, Streams pending length 급증, Pub/Sub lag 상승 등을 `/metrics`와 `redis-cli monitor`로 점검.
- AOF everysec + RDB snapshot을 병행하고, 장애시에는 Postgres를 통해 재구축한다.
- Pub/Sub 구독자가 없을 때 fallback 경로(로컬 브로드캐스트) 로그를 WARN 수준으로 남긴다.
- `gateway/session/*` 키 수를 SCAN해 sticky 세션이 정상 갱신되는지 모니터링한다.

## 장애 대비
- Redis 장애 시: 캐시 miss시 Postgres 직접 조회, Streams 대신 write-through로 전환(`WRITE_BEHIND_ENABLED=0`).
- TTL 설정 오류로 캐시가 자주 비면 `CACHE_TTL_*` 값을 늘리고, 필요 시 key-space notifications로 슬로우패스를 추적한다.
- Pub/Sub 장애 시: Streams 소비자로 대체하거나, Gateway간 direct gRPC 브리지로 임시 전환한다.
