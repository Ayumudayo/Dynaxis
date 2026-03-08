# 데이터베이스 마이그레이션(PostgreSQL)

본 디렉터리는 Dynaxis 서버의 기본 스키마를 구성하는 SQL 마이그레이션을 담습니다.

적용 순서(파일명 사전순)
- 0001_init.sql — 코어 테이블/확장
- 0002_indexes.sql — 인덱스/확장(pg_trgm), 일부는 CONCURRENTLY
- 0003_identity.sql — 이름 고유 제약 제거/세션 보조 컬럼/NULL 허용 정리
- 0004_session_events.sql — Write-behind용 세션 이벤트 테이블
  (옵션) 0100_seed_dev.sql — 로컬/스모크용 기본 데이터(lobby, 안내 메시지)

주의
- 0002_indexes.sql은 CREATE INDEX CONCURRENTLY를 포함합니다. 반드시 트랜잭션 블록 밖에서 실행하세요.
- 운영 환경에서는 큰 테이블에 대해 인덱스 생성 시간을 모니터링하고, 필요 시 배치 윈도우에서 실행하세요.

러너(선택)
- 별도 마이그레이션 러너가 `schema_migrations` 테이블로 버전을 관리하도록 권장합니다.
- 러너가 없다면 `psql`로 순서대로 적용해도 무방합니다.

예시(psql)
  psql "$DB_URI" -f tools/migrations/0001_init.sql
  psql "$DB_URI" -f tools/migrations/0002_indexes.sql   -- 트랜잭션 밖에서 실행
  psql "$DB_URI" -f tools/migrations/0003_identity.sql
  psql "$DB_URI" -f tools/migrations/0004_session_events.sql
  # 개발/스모크용 시드(옵션)
  psql "$DB_URI" -f tools/migrations/0100_seed_dev.sql
