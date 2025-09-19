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

## 향후 계획
- GoogleTest 기반 케이스 확대(Users/Rooms/Messages/Memberships 전 경로)
- Redis 의존 경로 통합 테스트(Docker Compose 병행)
- CI 매트릭스 구성(Windows/Linux, Debug/Release)

