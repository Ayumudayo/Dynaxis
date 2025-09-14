# 0002_indexes.sql (초안)

대규모 테이블에서도 락을 최소화하기 위해 가능한 경우 `CONCURRENTLY`를 사용한다. 해당 구문은 트랜잭션 블록 내에서 실행할 수 없으므로, 러너가 파일 단위 트랜잭션을 생략하거나 이 파일만 예외 처리해야 한다.

```sql
-- 확장 (유사 검색용)
create extension if not exists pg_trgm;

-- memberships 조회 성능
create index concurrently if not exists idx_memberships_room on memberships(room_id, user_id);
create index concurrently if not exists idx_memberships_user on memberships(user_id, room_id);

-- messages 페이징/히스토리
create index concurrently if not exists idx_messages_room_id_id on messages(room_id, id);
create index concurrently if not exists idx_messages_user_id_created on messages(user_id, created_at);

-- rooms 이름 검색(대소문자 무시)
create index concurrently if not exists idx_rooms_name_ci on rooms (lower(name));
create index concurrently if not exists idx_rooms_name_trgm on rooms using gin (lower(name) gin_trgm_ops);

-- users 이름 검색(대소문자 무시)
create index concurrently if not exists idx_users_name_ci on users (lower(name));
create index concurrently if not exists idx_users_name_trgm on users using gin (lower(name) gin_trgm_ops);

-- sessions 만료/사용자 조회
create index concurrently if not exists idx_sessions_user on sessions(user_id, created_at);
create index concurrently if not exists idx_sessions_expires on sessions(expires_at);
```

비고
- 개발/소규모 환경에서는 `concurrently` 없이도 무방하나, 운영에서는 권장
- 러너는 본 파일을 트랜잭션 없이 실행하도록 지원 필요
