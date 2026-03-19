# Core API Checklists

Use this document for both PR review and release-time validation when a change touches the public `server_core` API surface.

권장 읽기 순서는 `overview -> quickstart -> compatibility-policy -> checklists -> docs/tests`입니다. 이 문서는 그중 PR/release gate만 좁게 정리합니다.

## PR Review Checklist

- [ ] API 범위가 `docs/core-api-boundary.md`의 분류와 일치한다.
- [ ] 소유권/수명주기 계약이 명시적이며 일관된다.
- [ ] 스레드 안전성과 오류 동작이 문서화되어 있다.
- [ ] 호환성 영향이 `Stable` / `Transitional` / `Internal`로 분류되어 있다.
- [ ] 파괴적 변경에 대한 마이그레이션 노트가 추가되어 있다.
- [ ] `docs/core-api/` 하위 도메인 문서가 같은 PR에서 갱신되었다.
- [ ] 공개 API 스모크 소비자 build/run 검증이 성공한다.
- [ ] 확장 ABI 영향이 분류되어 있고 호환성 전략이 문서화되어 있다.
- [ ] 확장 관련 변경이 `docs/core-api/extensions.md`와 `docs/extensibility/*` 현재 계약 문서와 정합적이다.

## Release Checklist

### Scope

- `core/include/server/core/`의 `Stable` 헤더를 건드린 변경을 배포할 때 적용한다.

### Required Inputs

- 대상 릴리스 버전(`major.minor.patch`)
- 변경 집합(PR 목록 또는 커밋 범위)
- `docs/core-api/` 하위의 필수 마이그레이션 노트

### Release Gates

- [ ] `core/include/server/core/api/version.hpp`가 이번 릴리스 버전에 맞게 갱신됨
- [ ] `docs/core-api/compatibility-matrix.json`의 `api_version`이 `version.hpp`와 일치함
- [ ] 파괴적 `Stable` API 변경에 대해 `docs/core-api/migration-note-template.md` 형식의 마이그레이션 노트가 존재함
- [ ] `docs/core-api/changelog.md`에 이번 릴리스 항목이 존재함
- [ ] 공개 소비자 타깃 build/run 검증 성공:
  - `core_public_api_smoke`
  - `core_public_api_headers_compile`
  - `core_public_api_stable_header_scenarios`
  - `core_public_api_extensibility_smoke`
  - `core_public_api_realtime_capability_smoke`
  - `CoreInstalledPackageConsumer` (install/configure/build + consumer executables run)
  - `pwsh scripts/run_linux_installed_consumer.ps1` (Linux container parity smoke)
- [ ] compatibility wrapper를 유지하는 rename/deprecation tranche라면 `CoreFpsCompatSmoke`가 통과함
- [ ] API 거버넌스 검증 통과:
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`

### Verification Commands

```powershell
cmake --build build-windows --config Debug --target `
  core_public_api_smoke `
  core_public_api_headers_compile `
  core_public_api_stable_header_scenarios `
  core_public_api_extensibility_smoke `
  core_public_api_realtime_capability_smoke `
  core_fps_compat_smoke `
  --parallel

build-windows/tests/Debug/core_public_api_smoke.exe
build-windows/tests/Debug/core_public_api_headers_compile.exe
build-windows/tests/Debug/core_public_api_stable_header_scenarios.exe
build-windows/tests/Debug/core_public_api_extensibility_smoke.exe
build-windows/tests/Debug/core_public_api_realtime_capability_smoke.exe
build-windows/tests/Debug/core_fps_compat_smoke.exe

python tools/check_core_api_contracts.py --check-boundary
python tools/check_core_api_contracts.py --check-boundary-fixtures
python tools/check_core_api_contracts.py --check-stable-governance-fixtures

ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure

pwsh scripts/run_linux_installed_consumer.ps1
```

### Approval Record

- 릴리스 버전:
- 검토자:
- 일자(UTC):
- 비고:
