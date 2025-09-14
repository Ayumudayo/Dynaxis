# DB 아키텍처 및 연동

이 문서는 서버가 PostgreSQL과 연동되는 방식을 SOLID/비동기 친화적으로 정리한다. 사용자 승인 전까지 구현은 진행하지 않는다.

## 계층/네임스페이스
- SPI(인터페이스): `server::core::storage`
  - `IConnectionPool`, `IUnitOfWork`
  - 리포지토리: `IUserRepository`, `IRoomRepository`, `IMembershipRepository`, `IMessageRepository`, `ISessionRepository`
- 어댑터(Postgres): `server::storage::postgres`
  - `PgConnectionPool`, `PgUnitOfWork`, `Pg*Repository`
- 조립: `server::app::bootstrap`에서 풀과 리포지토리를 서비스에 주입

## 식별자(IDs)
- 시스템 식별자는 UUID이며, 이름은 라벨이다.
- 모든 경로/프로토콜/키/조인에서 UUID를 사용한다(`user_id`, `room_id`, `session_id`).

## 실행 모델
- DB 호출은 전용 스레드풀(기존 `JobQueue`/thread manager)을 사용하여 `io_context` 블로킹을 피한다.
- 각 핸들러(`on_login`, `on_join` 등)는 `IUnitOfWork`로 트랜잭션 경계를 정의한다.
- 결과는 세션 `strand`로 post하여 세션 상태 변경을 직렬화한다.

## 에러 매핑
- SQLSTATE를 프로토콜 에러로 매핑해 안전한 사용자 메시지를 제공한다.
- DB 상세 오류는 서버 로그에만 남긴다.

## 설정 키
- `DB_URI` (예: `postgres://user:pass@host:5432/app?sslmode=disable`)
- `DB_POOL_MIN` / `DB_POOL_MAX` (기본: `#CPU` ~ `2 * #CPU`)
- `DB_CONN_TIMEOUT_MS` (커넥션 대기)
- `DB_QUERY_TIMEOUT_MS` (스테이트먼트 타임아웃)
- `DB_HEALTHCHECK_INTERVAL_SEC`
- `DB_PREPARE_STATEMENTS` (bool)

## Redis 연동
- 역할: 캐시/세션/프레즌스/팬아웃, SoR는 Postgres 유지
- 사용처: 세션 TTL 저장, 프레즌스 집합, 룸 멤버십/최근 메시지 캐시, Pub/Sub·Streams 팬아웃, 레이트리밋, 아이도템포턴시 키
- 설정: `REDIS_URI`, `REDIS_POOL_MAX`, `REDIS_CHANNEL_PREFIX`, `REDIS_USE_STREAMS`, `REDIS_STREAM_MAXLEN`, `CACHE_TTL_*`
- 장애: Redis 장애 시 Postgres 폴백(캐시 미스 경로), 팬아웃은 인프로세스로 축소

## Write-Behind 옵션(세션)
- 세션 고빈도 이벤트를 Redis(Streams)에 버퍼링 후 Postgres에 배치 커밋
- 보장: at-least-once + 멱등 삽입, 분석/감사는 최종적 일관성
- 제어: `WRITE_BEHIND_ENABLED`, 배치 크기/지연, 워커 동시성, DLQ
- 자세한 내용: `docs/db/write-behind.md`

## 폴백/복구
- 운영 정책과 알람/런북: `docs/ops/fallback-and-alerts.md`
- 주요 플래그: `REDIS_REQUIRED`, `USE_REDIS_CACHE`, `USE_REDIS_PUBSUB`, `WRITE_BEHIND_ENABLED`
- 복구 후 캐시 프리워밍과 Streams 펜딩 처리 절차 정의

## Dependency Injection
- `bootstrap` creates a `PgConnectionPool` from env/config.
- Repositories are constructed per-request or shared as lightweight handles bound to the pool.
- Services receive repositories (or a repository factory) via constructor injection.

## Observability
- Emit metrics: pool utilization, acquire latency, query latency (avg/95p/99p), error rate.
- Tracing: add span/tags per handler with SQL timings.

## Future Extensions
- Outbox pattern: write broadcast events in the same DB transaction; a worker delivers them.
- Read replicas: optional read-only pool for fanout queries (join/room users/history).
