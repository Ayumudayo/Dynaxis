# 데이터베이스(PostgreSQL) 초기 설정(Provisioning)

새 Postgres 서버에 애플리케이션 전용 데이터베이스/유저/권한을 생성하는 초기 설정 스크립트입니다.

파일 구성
- 01_create_db_and_user.sql — `postgres` DB에 접속하여 역할(Role)/데이터베이스(Database)를 생성
- 02_init_db.sql — 새 DB에 접속하여 확장/권한/스키마를 초기화

사용 예시(psql)
1) DB/유저 생성 (슈퍼유저 권한 필요)
   psql -U postgres -h <host> -d postgres \
     -v APP_DB=dynaxis -v APP_USER=dynaxis_app -v APP_PASS='change_me' \
     -f tools/provisioning/01_create_db_and_user.sql

2) 새 DB 초기화(확장/권한/스키마)
   psql -U postgres -h <host> -d dynaxis \
     -v APP_DB=dynaxis -v APP_USER=dynaxis_app \
     -f tools/provisioning/02_init_db.sql

3) 스키마 마이그레이션/시드 적용(옵션)
   psql "$DB_URI" -f tools/migrations/0001_init.sql
   psql "$DB_URI" -f tools/migrations/0002_indexes.sql   -- 트랜잭션 밖
   psql "$DB_URI" -f tools/migrations/0003_identity.sql
   psql "$DB_URI" -f tools/migrations/0004_session_events.sql
   psql "$DB_URI" -f tools/migrations/0100_seed_dev.sql  -- 로컬/스모크용

비고
- 운영 환경에서는 `.env` 대신 Secret Manager/OS 환경변수 사용을 권장합니다.
- 패스워드는 반드시 강한 값으로 교체하세요.
