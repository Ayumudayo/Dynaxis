-- 0001_init.sql — Core schema initialization
-- UTF-8, 한국어 주석

-- 확장: gen_random_uuid()
create extension if not exists pgcrypto;

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

-- 마이그레이션 버전 테이블 (러너가 관리)
create table if not exists schema_migrations (
  version bigint primary key,
  applied_at timestamptz not null default now()
);
