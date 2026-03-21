# 코어 API 체크리스트

이 문서는 public `server_core` 표면을 건드리는 변경을 검토하거나 배포할 때 쓰는 기준 체크리스트다.

중요한 점은 이 문서가 단순한 형식 점검표가 아니라는 것이다. public API는 한번 흔들리면 설치형 소비자, 계약 테스트, 문서, 호환성 정책이 함께 어긋난다. 그래서 PR 단계와 release 단계에서 같은 기준을 반복해서 확인해야 유지보수가 쉬워진다.

권장 읽기 순서는 다음과 같다.

`overview -> quickstart -> compatibility-policy -> checklists -> docs/tests`

## PR 검토 체크리스트

- [ ] API 범위가 `docs/core-api-boundary.md`의 분류와 일치한다.
- [ ] 소유권, 수명주기, 실패 시 의미가 문서에 명시되어 있다.
- [ ] 스레드 안전성과 오류 동작이 구현과 문서에서 일관된다.
- [ ] 호환성 영향이 `Stable` / `Transitional` / `Internal` 중 어디에 속하는지 분류되어 있다.
- [ ] 파괴적 변경이면 마이그레이션 노트가 추가되어 있다.
- [ ] `docs/core-api/` 하위 도메인 문서가 같은 PR에서 함께 갱신되었다.
- [ ] 공개 API 스모크 소비자 build/run 검증이 성공한다.
- [ ] 확장 ABI 영향이 있다면 `docs/core-api/extensions.md`와 `docs/extensibility/*` 문서까지 함께 정합성이 맞는다.

이 체크리스트가 필요한 이유는, public API 변경이 코드 한 군데에서만 끝나지 않기 때문이다. 체크를 건너뛰면 문서는 옛 기준을 말하고 테스트는 새 기준을 기대하는 식의 drift가 생긴다.

## 릴리스 체크리스트

### 범위

- `core/include/server/core/`의 `Stable` 헤더를 건드린 변경을 배포할 때 적용한다.

### 필요한 입력

- 대상 릴리스 버전(`major.minor.patch`)
- 변경 집합(PR 목록 또는 커밋 범위)
- 필요한 마이그레이션 노트 경로

### 릴리스 게이트

- [ ] `core/include/server/core/api/version.hpp`가 이번 릴리스 버전에 맞게 갱신되어 있다.
- [ ] `docs/core-api/compatibility-matrix.json`의 `api_version`이 `version.hpp`와 일치한다.
- [ ] 파괴적 `Stable` API 변경이면 `docs/core-api/migration-note-template.md` 형식의 마이그레이션 노트가 존재한다.
- [ ] `docs/core-api/changelog.md`에 이번 릴리스 항목이 존재한다.
- [ ] 공개 소비자 타깃 build/run 검증이 성공한다.
  - `core_public_api_smoke`
  - `core_public_api_headers_compile`
  - `core_public_api_stable_header_scenarios`
  - `core_public_api_extensibility_smoke`
  - `core_public_api_realtime_capability_smoke`
  - `core_public_api_worlds_aws_smoke`
  - `CoreInstalledPackageConsumer`
  - `pwsh scripts/run_linux_installed_consumer.ps1`
- [ ] API 거버넌스 검증이 통과한다.
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`

이 단계가 중요한 이유는, public API는 “컴파일만 되면 된다”가 아니기 때문이다. 설치형 소비자, 문서, boundary fixture, stable governance fixture가 함께 맞아야 실제로 안전한 릴리스라고 볼 수 있다.

### 검증 명령

```powershell
cmake --build build-windows --config Debug --target `
  core_public_api_smoke `
  core_public_api_headers_compile `
  core_public_api_stable_header_scenarios `
  core_public_api_extensibility_smoke `
  core_public_api_realtime_capability_smoke `
  core_public_api_worlds_aws_smoke `
  --parallel

build-windows/tests/Debug/core_public_api_smoke.exe
build-windows/tests/Debug/core_public_api_headers_compile.exe
build-windows/tests/Debug/core_public_api_stable_header_scenarios.exe
build-windows/tests/Debug/core_public_api_extensibility_smoke.exe
build-windows/tests/Debug/core_public_api_realtime_capability_smoke.exe
build-windows/tests/Debug/core_public_api_worlds_aws_smoke.exe

python tools/check_core_api_contracts.py --check-boundary
python tools/check_core_api_contracts.py --check-boundary-fixtures
python tools/check_core_api_contracts.py --check-stable-governance-fixtures

ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure

pwsh scripts/run_linux_installed_consumer.ps1
```

### 승인 기록

- 릴리스 버전:
- 검토자:
- 일자(UTC):
- 비고:
