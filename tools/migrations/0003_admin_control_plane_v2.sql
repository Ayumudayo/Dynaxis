-- 0003_admin_control_plane_v2.sql — Web Admin Console v2 Control Plane Tables
-- UTF-8, 한국어 주석
-- 제재 이력, 운영 감사 로그, 쓰기 멱등성 저장소를 정의하며, 일반 채팅 평문은 저장/노출하지 않는다.

-- 관리자 제재(mute/ban) 원장 (chat_shared_state의 'penalty:<subject>'와 트랜잭션 단위로 동기화)
create table if not exists admin_sanctions (
  id uuid primary key default gen_random_uuid(),
  subject text not null,
  kind text not null check (kind in ('mute', 'ban')),
  reason text not null,
  actor_role text not null,
  created_at timestamptz not null default now(),
  expires_at timestamptz not null,
  revoked_at timestamptz,
  revoked_role text,
  revoked_reason text
);

create index if not exists idx_admin_sanctions_subject_expires
  on admin_sanctions (subject, expires_at desc);

create index if not exists idx_admin_sanctions_expires
  on admin_sanctions (expires_at desc);

-- 운영 제어면 감사 로그 (허용·거절·멱등 충돌 이력 추적)
create table if not exists admin_audit_logs (
  id bigserial primary key,
  created_at timestamptz not null default now(),
  actor_role text not null,
  capability text not null,
  action text not null,
  target text not null,
  reason text not null default '',
  idempotency_key text,
  status text not null,
  summary jsonb not null default '{}'::jsonb
);

create index if not exists idx_admin_audit_logs_created
  on admin_audit_logs (created_at desc, id desc);

create index if not exists idx_admin_audit_logs_target_created
  on admin_audit_logs (target, created_at desc);

create index if not exists idx_admin_audit_logs_action_created
  on admin_audit_logs (action, created_at desc);

-- 쓰기 API 멱등성 키 원장 (동일 키/해시 재전송 시 저장된 응답 재반환, 내용 불일치 시 409 충돌)
create table if not exists admin_idempotency_keys (
  idempotency_key text primary key,
  endpoint text not null,
  request_hash text not null,
  http_status integer not null,
  response_json jsonb not null,
  created_at timestamptz not null default now()
);

create index if not exists idx_admin_idempotency_keys_created
  on admin_idempotency_keys (created_at desc);
