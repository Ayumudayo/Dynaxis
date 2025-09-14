-- Simple initial setup for PostgreSQL
-- Goal: Create database mychatserver and core schema/tables/indexes.
-- Note:
--  1) Run the first statement to create the database.
--  2) Connect to the new database (MyChatServer) in your client (e.g., HeidiSQL),
--     then run the rest in the same file starting from the section marker.

-- STEP 1: Create database (lowercase, unquoted)
CREATE DATABASE mychatserver ENCODING 'UTF8' TEMPLATE template0 LC_COLLATE 'C' LC_CTYPE 'C';

-- ===== Reconnect to DB: mychatserver =====
-- From this point, execute after you have connected to the mychatserver database.

-- Safety guard: abort if not connected to the expected database
DO
$$
BEGIN
  IF current_database() <> 'mychatserver' THEN
    RAISE EXCEPTION 'Not connected to mychatserver (current: %). Connect to mychatserver and rerun the rest of this file.', current_database();
  END IF;
END
$$ LANGUAGE plpgsql;

-- STEP 2: Extensions
CREATE EXTENSION IF NOT EXISTS pgcrypto; -- gen_random_uuid()
CREATE EXTENSION IF NOT EXISTS pg_trgm;  -- trigram search

-- STEP 3: Tables
CREATE TABLE IF NOT EXISTS users (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  name text NOT NULL,
  password_hash text NOT NULL,
  last_login_ip inet,
  last_login_at timestamptz,
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS rooms (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  name text NOT NULL,
  is_public boolean NOT NULL DEFAULT true,
  is_active boolean NOT NULL DEFAULT true,
  closed_at timestamptz,
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS memberships (
  user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  room_id uuid NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  role text NOT NULL DEFAULT 'member',
  joined_at timestamptz NOT NULL DEFAULT now(),
  last_seen_msg_id bigint,
  is_member boolean NOT NULL DEFAULT true,
  left_at timestamptz,
  PRIMARY KEY (user_id, room_id)
);

CREATE TABLE IF NOT EXISTS messages (
  id bigserial PRIMARY KEY,
  room_id uuid NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  room_name text,
  user_id uuid REFERENCES users(id) ON DELETE SET NULL,
  content text NOT NULL,
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS sessions (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id uuid NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash bytea NOT NULL UNIQUE,
  client_ip inet,
  user_agent text,
  created_at timestamptz NOT NULL DEFAULT now(),
  expires_at timestamptz NOT NULL,
  revoked_at timestamptz
);

-- Optional: write-behind session events (can be skipped if not used)
CREATE TABLE IF NOT EXISTS session_events (
  id bigserial PRIMARY KEY,
  event_id text UNIQUE NOT NULL,
  type text NOT NULL,
  ts timestamptz NOT NULL,
  user_id uuid,
  session_id uuid,
  room_id uuid,
  payload jsonb
);

-- STEP 4: Indexes (non-concurrently for simplicity)
CREATE INDEX IF NOT EXISTS idx_memberships_room ON memberships(room_id, user_id);
CREATE INDEX IF NOT EXISTS idx_memberships_user ON memberships(user_id, room_id);

CREATE INDEX IF NOT EXISTS idx_messages_room_id_id ON messages(room_id, id);
CREATE INDEX IF NOT EXISTS idx_messages_user_id_created ON messages(user_id, created_at);

CREATE INDEX IF NOT EXISTS idx_rooms_name_ci ON rooms (lower(name));
CREATE INDEX IF NOT EXISTS idx_rooms_name_trgm ON rooms USING gin (lower(name) gin_trgm_ops);

CREATE INDEX IF NOT EXISTS idx_users_name_ci ON users (lower(name));
CREATE INDEX IF NOT EXISTS idx_users_name_trgm ON users USING gin (lower(name) gin_trgm_ops);

CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id, created_at);
CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);

CREATE INDEX IF NOT EXISTS idx_session_events_user_ts ON session_events(user_id, ts);
CREATE INDEX IF NOT EXISTS idx_session_events_session_ts ON session_events(session_id, ts);
CREATE INDEX IF NOT EXISTS idx_session_events_type_ts ON session_events(type, ts);

-- STEP 5: Minimal seed (optional)
INSERT INTO rooms (id, name, is_public, is_active, created_at)
SELECT gen_random_uuid(), 'lobby', true, true, now()
WHERE NOT EXISTS (SELECT 1 FROM rooms WHERE lower(name)=lower('lobby'));

INSERT INTO messages (room_id, user_id, content, created_at)
SELECT r.id, NULL, 'Welcome to lobby!', now()
FROM rooms r
WHERE lower(r.name) = lower('lobby')
  AND NOT EXISTS (
    SELECT 1 FROM messages m WHERE m.room_id = r.id AND m.content = 'Welcome to lobby!'
  );
