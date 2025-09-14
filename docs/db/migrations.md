# 마이그레이션 전략

버전형 SQL 파일과 경량 러너를 사용한다. 본 문서는 방식만 정의하며, 구현은 보류한다.

## 레이아웃
- 디렉터리: `tools/migrations/`
- 네이밍: `0001_init.sql`, `0002_indexes.sql`, `0003_sessions.sql` ...
- 적용 순서: 파일명 사전순, 각 파일은 1회 적용

## 러너 책임
- 현재 버전을 `schema_migrations`에서 조회
- 보류 파일마다 트랜잭션 시작 → 스크립트 실행 → `schema_migrations(version)` 기록 후 커밋
- 실패 시 롤백하며 버전 기록 금지
- 옵션: `--dry-run`으로 계획 출력

## 작성 가이드
- 대상 버전에서 멱등성을 지킬 것(`if not exists` 활용)
- 삭제/파괴 작업은 사전 백업과 별도 플래그 없이 진행하지 않음
- 큰 변경은 다단계로 분할
- 장시간 작업은 배치/측정으로 조정

## `0001_init.sql` 예시 범위
- 코어 테이블(users, rooms, memberships, messages, sessions, schema_migrations)
- 기본 인덱스

## 롤백
- 자동화하지 않음. 필요한 경우 별도 down 스크립트를 작성해 검토 후 적용

## 제안 스크립트 구성(초안)
- 0001_init.sql — 코어 스키마 생성
  - 내용: docs/db/migrations/0001_init.sql.md 참조
- 0002_indexes.sql — 인덱스/확장(운영은 CONCURRENTLY)
  - 내용: docs/db/migrations/0002_indexes.sql.md 참조
  - 주의: CONCURRENTLY는 트랜잭션 블록 밖에서 실행해야 함
- 0003_identity.sql — 이름 고유 제약 제거, 컬럼 추가(rooms.is_active/closed_at, sessions.client_ip/user_agent)
  - 내용: docs/db/migrations/0003_identity.sql.md 참조
- 0004_session_events.sql — write-behind용 이벤트 영속 테이블
  - 내용: docs/db/migrations/0004_session_events.sql.md 참조

## 트랜잭션/락 가이드
- 기본 원칙: 파일 단위 트랜잭션으로 원자성 보장
- 예외: `CREATE INDEX CONCURRENTLY`는 트랜잭션 외부에서 실행. 러너에 예외 플래그 필요
- 운영 환경에서는 `lock_timeout`, `statement_timeout`을 적절히 설정
