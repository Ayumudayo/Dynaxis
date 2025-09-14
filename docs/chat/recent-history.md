# Recent Room History on Join (Redis-backed)

목표: 사용자가 방에 접속할 때 최근 채팅 20건을 빠르게 로드하여 표시하고, 순서/중복 문제를 방지하기 위해 스냅샷 완료 전까지 입력을 비활성화한다.

## 데이터 원천 및 키
- SoR: Postgres(`messages` 테이블, `id`=bigserial, 시간/순서 기준)
- Redis 캐시:
  - `room:{room_id}:recent` — LIST/ZSET of `message_id` (최신 → 오래된 순)
  - `msg:{message_id}` — JSON 직렬화된 메시지 본문(`id, room_id, user_id, content, created_at`)
  - TTL: `CACHE_TTL_RECENT_MSGS`(예: 1–6h)

## 서버 알고리즘(Join 시)
1) 워터마크 결정
- `WM =` 현재 시점의 최고 메시지 id(가능하면 `MAX(id)`를 캐시하여 O(1)로 읽음)

2) Redis에서 최근 20개 id 조회
- `LRANGE room:{room_id}:recent -20 -1` (또는 `ZREVRANGE` 기준)
- 결과 id 집합을 오름차순 정렬하여 안정된 순서 보장

3) 메시지 본문 조회
- `MGET msg:{id}...` 또는 `pipeline HGETALL`
- 누락(gap) 발생 시 해당 id 범위를 Postgres에서 `SELECT ... WHERE room_id=? AND id IN (...)`로 로드 → Redis에 `SETEX msg:{id}` 및 `LPUSH/LTRIM`으로 캐시 보강

4) 스냅샷 패킷 전송
- 프로토콜: `MSG_STATE_SNAPSHOT`를 사용하거나, `MSG_ROOM_SNAPSHOT_BEGIN`/`MSG_ROOM_SNAPSHOT_END` 커맨드를 도입(문서 단계)
- 페이로드: `[messages...]`(id 오름차순), `room_id`, `wm`(워터마크)

5) 이후 도착 메시지 처리
- 스냅샷 이후 브로드캐스트로 오는 메시지 중 `id <= wm`은 중복으로 간주하여 드랍(클라이언트/서버 한쪽에서 멱등 처리)
- `id > wm`만 신규로 표시

6) 캐시 갱신(write-through)
- 신규 메시지 수신 시 Redis에 `LPUSH room:{room_id}:recent id` 후 `LTRIM ... 0 199` 등으로 길이 제한
- `SETEX msg:{id}`로 본문 캐시

## 클라이언트(UI) 게이팅
- 방 입장 요청 전송 후 즉시 입력창/보내기 버튼 비활성화
- `snapshot_complete` 신호(스냅샷 패킷 수신 완료)까지 비활성화 유지
- 중복 방지를 위해 클라이언트는 마지막 표시 `max_id`를 추적, `id <= max_id`는 무시

## 장애/폴백
- Redis 미스/장애: Postgres에서 최근 20건 쿼리(`ORDER BY id DESC LIMIT 20`) 후 역정렬하여 전송 → 선택적으로 Redis 캐시 재구축
- 과부하 시: 최근 10건으로 축소, TTL 단축

## 구성 값
- `RECENT_HISTORY_LIMIT`(기본 20)
- `CACHE_TTL_RECENT_MSGS`
- `ROOM_RECENT_MAXLEN`(예: 200)

## 보안/프라이버시
- 메시지 본문 JSON에 민감 정보 저장 금지. 필요 시 마스킹/검열 필터를 서버 측에서 먼저 적용 후 캐시

## 근거/트레이드오프
- 장점: 빠른 체감 로딩, RDB 부하 감소
- 단점: 캐시 일관성 관리 비용, 복구 시 Postgres 쿼리 필요

