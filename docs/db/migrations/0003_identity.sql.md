# 0003_identity.sql (초안)

기존 스키마에서 이름 고유 제약을 제거하고, UUID 기반 식별을 강화한다. 또한 세션에 클라이언트 속성을 추가한다.

```sql
-- users.name 고유 제약 제거(기존에 존재하는 경우)
-- 표준 제약명 가정: users_name_key
alter table if exists users drop constraint if exists users_name_key;

-- rooms.name 고유 제약 제거(기존에 존재하는 경우)
-- 제약 이름은 환경마다 다를 수 있으므로, 정보 스키마 조회 후 동적 실행 또는 표준 이름을 가정한다.
-- 표준 이름 가정 예: rooms_name_key
alter table if exists rooms drop constraint if exists rooms_name_key;

-- rooms 컬럼 추가
alter table if exists rooms
  add column if not exists is_active boolean not null default true,
  add column if not exists closed_at timestamptz;

-- sessions 컬럼 추가
alter table if exists sessions
  add column if not exists client_ip inet,
  add column if not exists user_agent text;

-- messages.user_id는 사용자 삭제 시 기록 보존을 위해 NULL 허용
alter table if exists messages alter column user_id drop not null;
```

비고
- 이름 검색 인덱스는 0002에서 생성된다
- 다운그레이드(롤백) 시 데이터 충돌 위험이 높으므로 권장하지 않음
