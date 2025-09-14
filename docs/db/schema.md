# 논리 데이터 모델 및 DDL 초안

핵심 채팅 기능을 위한 1차 스키마다. 구현 과정에서 정교화한다.

## 엔티티
- users: 가입 사용자
- rooms: 채팅 방(공개/비공개)
- memberships: 방 내 사용자 멤버십
- messages: 방 내 메시지
- sessions: 로그인 세션/토큰
- schema_migrations: 마이그레이션 버전 관리

## DDL 초안(PostgreSQL)

```sql
-- 0001_init.sql (excerpt)
create extension if not exists pgcrypto; -- for gen_random_uuid()

create table if not exists users (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  password_hash text not null,
  created_at timestamptz not null default now()
);

create table if not exists rooms (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  is_public boolean not null default true,
  is_active boolean not null default true,
  closed_at timestamptz,
  created_at timestamptz not null default now()
);

create table if not exists memberships (
  user_id uuid not null references users(id) on delete cascade,
  room_id uuid not null references rooms(id) on delete cascade,
  role text not null default 'member',
  joined_at timestamptz not null default now(),
  last_seen_msg_id bigint,
  primary key (user_id, room_id)
);

create table if not exists messages (
  id bigserial primary key,
  room_id uuid not null references rooms(id) on delete cascade,
  user_id uuid references users(id) on delete set null,
  content text not null,
  created_at timestamptz not null default now()
);

create index if not exists idx_messages_room_id_id on messages(room_id, id);
create index if not exists idx_messages_user_id_created on messages(user_id, created_at);

create table if not exists sessions (
  id uuid primary key default gen_random_uuid(),
  user_id uuid not null references users(id) on delete cascade,
  token_hash bytea not null unique,
  client_ip inet,
  user_agent text,
  created_at timestamptz not null default now(),
  expires_at timestamptz not null,
  revoked_at timestamptz
);

create table if not exists schema_migrations (
  version bigint primary key,
  applied_at timestamptz not null default now()
);
-- 선택 인덱스/확장
create extension if not exists pg_trgm;
create index if not exists idx_rooms_name_ci on rooms (lower(name));
create index if not exists idx_rooms_name_trgm on rooms using gin (lower(name) gin_trgm_ops);
-- 사용자명 검색(대소문자 무시/유사 검색)
create index if not exists idx_users_name_ci on users (lower(name));
create index if not exists idx_users_name_trgm on users using gin (lower(name) gin_trgm_ops);
```

## 비고
- `messages.user_id on delete set null`: 사용자 삭제 시에도 기록 보존
- `sessions.token_hash`: 토큰 평문 대신 해시 저장(보안 문서 참고)
- 룸 이름은 고유 식별자가 아니다(중복 허용). 시스템 식별자는 `room_id(UUID)`
- 사용자명은 고유 식별자가 아니다(중복 허용). 시스템 식별자는 `user_id(UUID)`
- 세션은 `id(UUID)`로 식별하며, `client_ip`는 운영/보안 분석용 보조 속성
- 전문 검색(FTS), 보관 정책은 이후 정책/테이블로 확장 가능
