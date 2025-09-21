# 테스트 가이드

## 단위 테스트(저장소 Happy-path)
- 현재 기본 테스트는 Postgres 저장소 경로를 대상으로 합니다.
- 타깃: `storage_basic_tests`

### 실행 준비
- 환경 변수 `DB_URI` 설정(또는 `.env`에 기입)
- 스키마: `docs/db/migrations/*.sql.md`를 반영

### 빌드/실행(Windows PowerShell)
```
scripts/build.ps1 -Config Debug -BuildDir build-msvc -Target storage_basic_tests
build-msvc/tests/Debug/storage_basic_tests.exe
```

DB_URI 미설정이나 헬스체크 실패 시 테스트는 자동으로 skip 됩니다.

## 통합 스모크(Write-behind)
Streams→워커→Postgres 파이프라인을 간단히 검증합니다.

```
scripts/smoke_wb.ps1 -Config Debug -BuildDir build-msvc
```

내부 절차: wb_worker 백그라운드 → wb_emit(XADD) → wb_check(DB 확인) → 종료.

## E2E 수동 검증(서버/클라이언트)
- 준비: 루트 `.env`에 `WRITE_BEHIND_ENABLED=1`, Redis/DB URI 유효 값 설정. 각 바이너리 폴더에 `.env` 복사.
- 실행(원클릭)
  - Windows: `scripts/run_all.ps1 -Config Debug -WithClient`
  - WSL/Linux: `bash scripts/run_all.sh Debug build-linux 5000`
- 절차
  1) 클라이언트에서 로그인 → 룸 입장/퇴장 몇 회 수행
  2) 워커 로그의 `wb_flush`/`wb_pending` 변화 확인
  3) DB에서 최근 이벤트 확인:
     - `select id,event_id,type,ts,user_id,session_id,room_id from session_events order by id desc limit 20;`
     - 기대: `user_id/room_id`는 실제 UUID, `session_id`는 세션별 UUID(v4)

## 메트릭 확인(옵션)
- 서버: `.env`에 `METRICS_PORT=9090` 설정 후 `curl http://127.0.0.1:9090/metrics`
- 주요 지표: `chat_subscribe_total`, `chat_self_echo_drop_total`, `chat_subscribe_last_lag_ms`

## 향후 계획
- GoogleTest 기반 케이스 확대(Users/Rooms/Messages/Memberships 전 경로)
- Redis 의존 경로 통합 테스트(Docker Compose 병행)
- CI 매트릭스 구성(Windows/Linux, Debug/Release)

