-- 0001_schema.sql — Consolidated Initial Schema
-- UTF-8, 한국어 주석

-- 확장: gen_random_uuid(), pg_trgm
create extension if not exists pgcrypto;
create extension if not exists pg_trgm;

-- users
create table if not exists users (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  password_hash text not null,
  last_login_ip inet,
  last_login_at timestamptz,
  created_at timestamptz not null default now()
);

-- rooms (이름 중복 허용, 라벨 역할)
create table if not exists rooms (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  is_public boolean not null default true,
  is_active boolean not null default true,
  closed_at timestamptz,
  created_at timestamptz not null default now()
);

-- memberships (복합 PK)
create table if not exists memberships (
  user_id uuid not null references users(id) on delete cascade,
  room_id uuid not null references rooms(id) on delete cascade,
  role text not null default 'member',
  joined_at timestamptz not null default now(),
  last_seen_msg_id bigint,
  is_member boolean not null default true,
  left_at timestamptz,
  primary key (user_id, room_id)
);

-- messages
create table if not exists messages (
  id bigserial primary key,
  room_id uuid not null references rooms(id) on delete cascade,
  room_name text,
  user_id uuid references users(id) on delete set null,
  content text not null,
  created_at timestamptz not null default now()
);

-- sessions
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

-- session_events
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

-- 마이그레이션 버전 테이블 (러너가 관리)
create table if not exists schema_migrations (
  version bigint primary key,
  applied_at timestamptz not null default now()
);

-- Indexes
-- 초기 스키마 생성 시에는 데이터가 없으므로 CONCURRENTLY를 사용하지 않음 (트랜잭션 호환성)

-- memberships 조회 성능
create index if not exists idx_memberships_room on memberships(room_id, user_id);
create index if not exists idx_memberships_user on memberships(user_id, room_id);

-- messages 페이징/히스토리
create index if not exists idx_messages_room_id_id on messages(room_id, id);
create index if not exists idx_messages_user_id_created on messages(user_id, created_at);

-- rooms 이름 검색(대소문자 무시/유사 검색)
create index if not exists idx_rooms_name_ci on rooms (lower(name));
create index if not exists idx_rooms_name_trgm on rooms using gin (lower(name) gin_trgm_ops);

-- users 이름 검색(대소문자 무시/유사 검색)
create index if not exists idx_users_name_ci on users (lower(name));
create index if not exists idx_users_name_trgm on users using gin (lower(name) gin_trgm_ops);

-- sessions 만료/사용자 조회
create index if not exists idx_sessions_user on sessions(user_id, created_at);
create index if not exists idx_sessions_expires on sessions(expires_at);

-- session_events indexes
create index if not exists idx_session_events_user_ts on session_events(user_id, ts);
create index if not exists idx_session_events_session_ts on session_events(session_id, ts);
create index if not exists idx_session_events_type_ts on session_events(type, ts);
