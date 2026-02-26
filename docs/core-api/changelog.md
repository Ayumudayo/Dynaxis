# 코어(Core) API 변경 이력

주요 `Stable` API 변경은 이 파일에 기록합니다.

## 형식
- 각 릴리스 항목은 `변경됨(Changed)`, `파괴적 변경(Breaking)`, `사용 중단(Deprecated)` 구역을 사용합니다.
- 파괴적 변경 항목에는 `docs/core-api/` 하위 마이그레이션 노트 경로를 포함해야 합니다.

## 미출시(Unreleased)

### 변경됨(Changed)
- 헤더별 소비자 사용 커버리지를 높이기 위해 `CorePublicApiStableHeaderScenarios` 테스트 타깃을 추가했습니다.
- `tools/check_core_api_contracts.py`에 stable-governance fixture 회귀 검증을 추가했습니다.

### 파괴적 변경(Breaking)
- 없음

### 사용 중단(Deprecated)
- 없음
