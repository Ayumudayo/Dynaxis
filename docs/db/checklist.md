# 구현 전 체크리스트(DB)

구현 착수 전 아래 항목을 확인한다. 승인 전까지 코드 변경 없음.

## 결정 사항
- [x] DB 엔진: PostgreSQL 16
- [x] 드라이버: libpqxx
- [x] 비밀번호 해시: Argon2id
- [x] Docker 사용: 선택, 기본은 외부 DB
- [x] 확장: 설정 기반 튜닝(코드 샤딩 미도입)
- [x] Redis: 캐시/세션/팬아웃(SoR는 Postgres)
- [x] 식별자: UUID 기반, 이름은 라벨(룸 이름 중복 허용)

## 설계 문서 현황
- [x] ADR(결정 기록)
- [x] 아키텍처/연동 계획
- [x] 논리 스키마/DDL 초안
- [x] 마이그레이션 전략
- [x] 보안/운영
- [x] 로컬 개발 가이드
- [x] 프로토콜: 스냅샷 옵션/포맷
- [x] 시퀀스: join → snapshot → fanout
- [x] 운영: Redis 폴백/복구

## 설정 키
- [x] `DB_URI`
- [x] `DB_POOL_MIN` / `DB_POOL_MAX`
- [x] `DB_CONN_TIMEOUT_MS`
- [x] `DB_QUERY_TIMEOUT_MS`
- [x] `DB_HEALTHCHECK_INTERVAL_SEC`
- [x] `DB_PREPARE_STATEMENTS`
- [x] `REDIS_URI`
- [x] `REDIS_POOL_MAX`
- [x] `REDIS_CHANNEL_PREFIX`
- [x] `REDIS_USE_STREAMS`, `REDIS_STREAM_MAXLEN`
- [x] `CACHE_TTL_SESSION`, `CACHE_TTL_RECENT_MSGS`, `CACHE_TTL_MEMBERS`
- [x] `WRITE_BEHIND_ENABLED`, `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_MAX_DELAY_MS`, `WB_WORKER_CONCURRENCY`
- [x] `REDIS_REQUIRED`, `USE_REDIS_CACHE`, `USE_REDIS_PUBSUB`

## Go/No-Go
- [ ] 구현 단계 승인 시 진행할 항목:
  - core 저장소 SPI
  - Postgres 어댑터/풀
  - Redis 클라이언트/풀 및 캐시·세션 연동
  - Write-behind 워커(Streams 컨슈머) 및 멱등 커밋
  - 핸들러 주입/부트스트랩 배선
  - 마이그레이션/시드 스크립트
