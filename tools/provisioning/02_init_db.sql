-- 02_init_db.sql (pgAdmin4-friendly)
-- 목적: 현재 접속한 DB에서 확장/권한/스키마 기본 설정을 수행
-- 사용법: pgAdmin4의 Query Tool에서 대상 DB로 연결한 뒤 실행
-- 설정 대상 유저: 기본은 current_user. 다른 유저를 대상으로 하려면 아래 v_user 값을 수정

-- 확장: 애플리케이션이 기대하는 확장 설치
CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE EXTENSION IF NOT EXISTS pg_trgm;

DO
$$
DECLARE
  v_db   text := current_database();
  v_user text := current_user; -- 필요 시 'my_app_user' 등으로 수정
BEGIN
  -- DB 레벨: PUBLIC에 대한 과도한 권한 제한(선택)
  EXECUTE format('REVOKE ALL ON DATABASE %I FROM PUBLIC', v_db);
  EXECUTE format('GRANT CONNECT ON DATABASE %I TO %I', v_db, v_user);

  -- 스키마: public 사용, 불필요한 CREATE 권한 제한 및 USAGE 부여
  EXECUTE 'REVOKE CREATE ON SCHEMA public FROM PUBLIC';
  EXECUTE format('GRANT USAGE ON SCHEMA public TO %I', v_user);

  -- search_path(선택): 해당 DB에 한해 기본 search_path 지정
  EXECUTE format('ALTER ROLE %I IN DATABASE %I SET search_path = public', v_user, v_db);

  -- 기존 오브젝트 권한(안전 차원에서 명시 부여)
  EXECUTE format('GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO %I', v_user);
  EXECUTE format('GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO %I', v_user);

  -- 향후 생성 오브젝트의 기본 권한
  EXECUTE format('ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO %I', v_user);
  EXECUTE format('ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO %I', v_user);
END
$$ LANGUAGE plpgsql;
