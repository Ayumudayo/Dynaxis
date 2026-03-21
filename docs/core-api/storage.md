# 스토리지 실행(Storage Execution) API 가이드

## 안정성
- canonical stable 공개 헤더는 아래 네 개입니다.
  - `server/core/storage_execution/unit_of_work.hpp`
  - `server/core/storage_execution/connection_pool.hpp`
  - `server/core/storage_execution/db_worker_pool.hpp`
  - `server/core/storage_execution/retry_backoff.hpp`
- shared Redis client contract, concrete Redis/Postgres adapter, chat repository DTO/UoW 계층은 계속 `[Internal]` 또는 app-owned로 남습니다.

## stable surface 범위
- canonical `storage_execution/**` 헤더는 의도적으로 얇은 facade입니다. consumer는 이 경로를 public contract로 보고, underlying `storage/*` 헤더를 동등한 공개 API로 취급하면 안 됩니다.
- `IUnitOfWork`
  - 도메인 저장소 accessor를 포함하지 않는 generic commit/rollback transaction 경계입니다.
- `IConnectionPool` + `PoolOptions`
  - generic unit-of-work 생성과 health check를 노출하는 adapter boundary입니다.
- `DbWorkerPool`
  - generic `IUnitOfWork` seam 위에서 비동기 storage task를 실행하는 worker pool입니다.
- `RetryBackoffPolicy`
  - linear delay와 exponential full-jitter delay를 공용 계산으로 제공하는 retry helper입니다.

## 의도적 비승격 영역
- 채팅 도메인 repository 인터페이스와 DTO(`user/room/message/membership/session`)는 `server/storage/*`에 남습니다.
- shared Redis client contract과 concrete Redis factory는 gateway/server/tools 공용 internal seam으로 유지됩니다.
- concrete Postgres 연결 구현과 repository-aware factory는 `server/storage/*` app-owned seam으로 유지됩니다.
- stable consumer는 canonical `storage_execution` surface만 직접 사용하고, chat repository 계층이나 internal Redis client contract에 직접 의존하지 않아야 합니다.

## package-first 상태
- storage factory package milestone은 계속 아래 seam을 기준으로 유지합니다.
  - `server_storage_pg_factory`
  - `server_storage_redis_factory`
- 설치된 소비자는 아래 imported target 이름을 사용합니다.
  - `server_storage_pg_factory::server_storage_pg_factory`
  - `server_storage_redis_factory::server_storage_redis_factory`
- `server_storage_pg_factory` consumer는 generic connection-pool 옵션을 `server::core::storage_execution::PoolOptions`로 다룹니다.
- `server_storage_pg`, `server_storage_redis`는 monorepo 내부 compatibility umbrella로 남고, canonical package surface로 취급하지 않습니다.
- `server_state_redis_factory`는 외부 소비자 수요가 확인되기 전까지 패키지화 대상에서 제외합니다.

## 사용 규칙
- repository 호출은 generic `IUnitOfWork` 경계 내부에서 수행하되, repository accessor 자체는 app-owned UoW에서 가져옵니다.
- 비동기 DB 실행은 `DbWorkerPool` 같은 generic execution seam 뒤로 숨기고, 도메인 DTO 해석은 app/service 계층에 둡니다.
- retry/backoff는 app-local while-loop에 묻히지 않게 `RetryBackoffPolicy` helper를 재사용합니다.
