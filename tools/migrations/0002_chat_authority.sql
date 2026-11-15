-- 앱의 공유 제어 상태. 방 권한은 Redis TTL/로컬 메모리와 독립적으로 보존한다.
create table if not exists chat_shared_state (
    key text primary key,
    value jsonb not null
);

-- 과거 방에는 입증 가능한 owner가 없다. 첫 원격 접속자에게 owner를 부여하지 않는다.
-- 관리자가 정리/재생성할 수 있으며 새 방은 생성 트랜잭션에서 owner를 기록한다.
insert into chat_shared_state(key, value)
select 'room:' || lower(name), jsonb_build_object('owner', '', 'password', '', 'invites', '[]'::jsonb, 'active', true)
from rooms where is_active and name <> 'lobby'
on conflict (key) do nothing;
