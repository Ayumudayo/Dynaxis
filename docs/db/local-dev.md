# 로컬 개발 옵션(PostgreSQL)

기본은 외부 Postgres를 사용한다. Docker Compose는 편의상 옵션으로 요청 시 제공한다.

## 옵션 A: 외부 Postgres(권장)
1. DB/사용자 생성:
   - `create user app with password 'secret';`
   - `create database app owner app;`
2. `DB_URI` 예: `postgres://app:secret@localhost:5432/app?sslmode=disable`
3. 서버는 환경변수/설정으로 구성(구현은 승인 이후 진행)

## 옵션 B: Docker(선택)
- 필요 시 `postgres:16-alpine` 기반 `docker-compose.dev.yml`을 제공 가능
- 임시 실행 예:
  - `docker run --rm -p 5432:5432 -e POSTGRES_PASSWORD=secret -e POSTGRES_USER=app -e POSTGRES_DB=app postgres:16-alpine`

## 유틸리티
- `psql` 접속: `psql $DB_URI`
- 초기화 스크립트: `tools/migrations/`에 두고 경량 러너로 적용(마이그레이션 문서 참조)
