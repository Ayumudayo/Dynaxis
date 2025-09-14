-- 0007_users_last_login.sql — add last_login fields to users

alter table users
  add column if not exists last_login_ip inet,
  add column if not exists last_login_at timestamptz;
