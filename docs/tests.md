# 테스트 가이드

## 1. 스토리지 단위 테스트
- 대상: `storage_basic_tests` (Postgres + Repository 기본 검증)
- 준비
  - 환경 변수에 `DB_URI` 지정
  - `docs/db/migrations/*.sql`로 스키마 적용
- 실행 (PowerShell)
  ```
  scripts/build.ps1 -Config Debug -BuildDir build-windows -Target storage_basic_tests
  build-windows/tests/Debug/storage_basic_tests.exe
  ```
- DB 연결이 없으면 테스트는 자동 skip 된다.

## 2. Redis/Write-behind 스모크 테스트
`wb_worker`, `wb_emit`, `wb_check`를 묶어 Streams→DB 라운드트립을 검증한다.
```
scripts/smoke_wb.ps1 -Config Debug -BuildDir build-windows
```
- Redis/DB URI를 환경 변수로 지정하고 `WRITE_BEHIND_ENABLED=1` 상태에서 실행.
- 완료 후 `wb_check`가 `session_events` 테이블을 조회해 XADD→DB 반영 여부를 출력한다.

## 3. E2E (Gateway + Server + Client)
- 준비: Redis(필수) / Postgres(옵션)을 준비하고, 필요한 환경 변수를 설정한다. (`.env.example` 참고)
- 실행 (Docker stack, HAProxy 포함)
  - Windows: `scripts/deploy_docker.ps1 -Action up -Stack -Detached -Build`
  - WSL/Linux: `docker build -f Dockerfile.base -t knights-base . && docker compose -f docker/stack/docker-compose.yml up -d --build`
- 검증 순서
  1. 클라이언트에서 로그인/룸 입장/채팅/퇴장 수행 (HAProxy: `127.0.0.1:6000`)
  2. `wb_flush`, `wb_pending` 로그 모니터링
  3. Postgres `session_events` 확인: `select id,event_id,type,ts,user_id,session_id,room_id from session_events order by id desc limit 20;`

## 4. Redis 통합 테스트(권장)
- Docker Compose로 Redis/DB를 띄우고 `tests/redis_integration_tests`(추가 예정)를 작성해 Presence/Room membership 캐시를 검증한다.
- 주요 시나리오: Presence TTL 만료, Sticky Session 재바인딩, Streams pending 복구.

## 5. Observability 체크
- `.env`의 `METRICS_PORT`를 지정한 뒤 `curl http://127.0.0.1:<port>/metrics`로 확인.
- 핵심 지표: `chat_subscribe_total`, `chat_self_echo_drop_total`, `chat_subscribe_last_lag_ms`, `wb_batch_size`, `wb_commit_ms`, `wb_pending`.

## 6. CI 권장 플랜
- Windows/Linux, Debug/Release 매트릭스에서 스토리지/Redis 스모크 테스트 실행.
- 장기적으로 GoogleTest 기반 `users_tests`, `memberships_tests`, Redis 통합 테스트를 추가해 최소 happy-path를 커버한다.
