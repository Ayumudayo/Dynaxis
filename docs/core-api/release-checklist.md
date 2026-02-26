# 코어 API 릴리스 체크리스트

## 범위
- `core/include/server/core/`의 `Stable` 헤더를 건드린 변경을 배포할 때 적용합니다.

## 필수 입력
- 대상 릴리스 버전(`major.minor.patch`)
- 변경 집합(PR 목록 또는 커밋 범위)
- `docs/core-api/` 하위의 필수 마이그레이션 노트

## 릴리스 게이트
- [ ] `core/include/server/core/api/version.hpp`가 이번 릴리스 버전에 맞게 갱신됨
- [ ] `docs/core-api/compatibility-matrix.json`의 `api_version`이 `version.hpp`와 일치함
- [ ] 파괴적 `Stable` API 변경에 대해 `docs/core-api/migration-note-template.md` 형식의 마이그레이션 노트가 존재함
- [ ] `docs/core-api/changelog.md`에 이번 릴리스 항목이 존재함
- [ ] 공개 소비자 타깃 빌드 성공:
  - `core_public_api_smoke`
  - `core_public_api_headers_compile`
  - `core_public_api_stable_header_scenarios`
- [ ] API 거버넌스 검증 통과:
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`

## 검증 명령

```powershell
python tools/check_core_api_contracts.py --check-boundary
python tools/check_core_api_contracts.py --check-boundary-fixtures
python tools/check_core_api_contracts.py --check-stable-governance-fixtures

pwsh scripts/build.ps1 -Config Debug -Target core_public_api_smoke
pwsh scripts/build.ps1 -Config Debug -Target core_public_api_headers_compile
pwsh scripts/build.ps1 -Config Debug -Target core_public_api_stable_header_scenarios

ctest --preset windows-test -R "CorePublicApi|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure
```

## 승인 기록
- 릴리스 버전:
- 검토자:
- 일자(UTC):
- 비고:
