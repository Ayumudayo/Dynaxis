# 테스트 가이드

## 1. 스토리지 단위 테스트
- 대상: `storage_basic_tests` (Postgres + Repository 기본 검증)
- 준비
  - 환경 변수에 `DB_URI` 지정
  - `tools/migrations/*.sql` 또는 `migrations_runner`로 스키마 적용
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
  - 권장(Windows/WSL/Linux): `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
  - Observability 포함: `pwsh scripts/run_full_stack_observability.ps1`
- 검증 순서
  1. 클라이언트에서 로그인/룸 입장/채팅/퇴장 수행 (HAProxy: `127.0.0.1:6000`)
  2. `wb_flush`, `wb_pending` 로그 모니터링
  3. Postgres `session_events` 확인: `select id,event_id,type,ts,user_id,session_id,room_id from session_events order by id desc limit 20;`

## 4. Redis 통합 테스트(권장)
- Docker Compose로 Redis/DB를 띄우고 `tests/redis_integration_tests`(추가 예정)를 작성해 Presence/Room membership 캐시를 검증한다.
- 주요 시나리오: Presence TTL 만료, Sticky Session 재바인딩, Streams pending 복구.

## 5. Observability 체크
- `.env`의 `METRICS_PORT`를 지정한 뒤 아래로 확인:
  - `curl http://127.0.0.1:<port>/metrics`
  - `curl http://127.0.0.1:<port>/healthz`
  - `curl http://127.0.0.1:<port>/readyz`
- 핵심 지표(현재 구현 기준):
  - server_app: `knights_build_info`, `chat_session_active`, `chat_dispatch_latency_ms_*`, `chat_dispatch_opcode_named_total`, `chat_job_queue_depth`, `chat_subscribe_total`, `chat_subscribe_last_lag_ms`, `chat_hook_plugins_enabled`
  - wb_worker: `wb_pending`, `wb_flush_total`, `wb_flush_batch_size_last`, `wb_flush_commit_ms_last`

## 6. CI 권장 플랜
- 기본 required gate: `.github/workflows/ci.yml`
  - Windows: Release 빌드 + `ctest --preset windows-test`
  - Opcodes / wire / Doxygen: 코드젠/문서 최신 상태 강제
- Stack smoke: `.github/workflows/ci-stack.yml`
  - Linux: Docker stack baseline/off + runtime-on 기본 스모크
- Extensibility smoke: `.github/workflows/ci-extensibility.yml`
  - Linux: plugin/script 메트릭, 핫리로드, fallback/rollback 검증
- Hardening: `.github/workflows/ci-hardening.yml`
  - Linux: `linux-asan`(ASan/UBSan), fuzz, soak/perf 검증

## 7. Push 전 로컬 검증 게이트
- 큰 변경(빌드/의존성/워크플로우/다중 모듈 변경)은 push 전에 로컬 검증을 먼저 통과한다.
- 권장 기본 게이트:
  - `pwsh scripts/build.ps1 -Config Release`
  - `ctest --preset windows-test --output-on-failure`
- `core/include/server/core/**` 변경 시 추가 게이트:
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`
- Linux 경로는 Docker stack 로컬 검증 + 원격 GitHub CI 결과를 기준으로 판단한다.
