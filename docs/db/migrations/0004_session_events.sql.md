# 0004_session_events.sql (초안)

Redis write-behind를 위한 세션 이벤트 영속 테이블을 추가한다.

```sql
create table if not exists session_events (
  id bigserial primary key,
  event_id text unique not null, -- Redis Streams XADD ID 또는 멱등 키
  type text not null,
  ts timestamptz not null,
  user_id uuid,
  session_id uuid,
  room_id uuid,
  payload jsonb
);

create index if not exists idx_session_events_user_ts on session_events(user_id, ts);
create index if not exists idx_session_events_session_ts on session_events(session_id, ts);
create index if not exists idx_session_events_type_ts on session_events(type, ts);
```

비고
- 멱등 삽입을 위해 `event_id`를 고유로 유지한다
- 저장 용량 관리가 필요하면 파티셔닝 또는 TTL 정책(보관 기간) 도입 고려

