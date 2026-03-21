# 데이터베이스(PostgreSQL) 초기 설정

이 디렉터리는 새 PostgreSQL 서버에 애플리케이션 전용 데이터베이스, 사용자, 권한을 처음 만드는 초기 설정 스크립트를 담는다.

이 단계를 별도로 두는 이유는 단순하다. 스키마 마이그레이션과 "DB 자체를 만드는 작업"은 권한 수준과 실패 영향이 다르기 때문이다.

- DB/사용자 생성
  - 슈퍼유저 또는 그에 준하는 권한이 필요하다
- 애플리케이션 초기화
  - 확장, 권한, 기본 스키마를 준비한다
- 이후 마이그레이션
  - 앱 버전에 따라 점진적으로 schema를 올린다

이 셋을 한 번에 섞으면 운영자가 어느 단계에서 실패했는지 추적하기 어렵고, 권한이 과하게 넓어진 상태로 자동화를 만들 가능성도 커진다.

## 파일 구성

- `01_create_db_and_user.sql`
  - `postgres` 데이터베이스에 접속해 역할(role)과 데이터베이스를 생성한다
- `02_init_db.sql`
  - 새 데이터베이스에 접속해 확장, 권한, 기본 스키마를 초기화한다

## 사용 예시(`psql`)

1. DB와 사용자를 만든다. 이 단계는 슈퍼유저 권한이 필요하다.

```powershell
psql -U postgres -h <host> -d postgres `
  -v APP_DB=dynaxis -v APP_USER=dynaxis_app -v APP_PASS='change_me' `
  -f tools/provisioning/01_create_db_and_user.sql
```

2. 새 데이터베이스를 초기화한다.

```powershell
psql -U postgres -h <host> -d dynaxis `
  -v APP_DB=dynaxis -v APP_USER=dynaxis_app `
  -f tools/provisioning/02_init_db.sql
```

3. 필요한 마이그레이션과 시드를 적용한다. 이 단계는 앱 버전에 따라 달라질 수 있다.

```powershell
psql "$DB_URI" -f tools/migrations/0001_init.sql
psql "$DB_URI" -f tools/migrations/0002_indexes.sql
psql "$DB_URI" -f tools/migrations/0003_identity.sql
psql "$DB_URI" -f tools/migrations/0004_session_events.sql
psql "$DB_URI" -f tools/migrations/0100_seed_dev.sql
```

`0002_indexes.sql`는 `CREATE INDEX CONCURRENTLY`를 포함하므로 트랜잭션 바깥에서 실행해야 한다는 점을 잊지 않는 편이 좋다.

## 운영 메모

- 운영 환경에서는 `.env`보다 Secret Manager나 OS 환경 변수를 우선하는 편이 안전하다
- 예시 비밀번호는 반드시 강한 값으로 바꿔야 한다
- "초기 설정"과 "버전별 마이그레이션"을 분리해 두면, 새 서버 bootstrap과 앱 업그레이드 절차를 각각 독립적으로 반복할 수 있다
