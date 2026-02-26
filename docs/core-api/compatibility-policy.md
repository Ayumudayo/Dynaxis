# 호환성 정책(server_core)

## 적용 범위
- `docs/core-api-boundary.md`에서 `Stable`로 지정한 헤더에 적용합니다.
- `Transitional`, `Internal` 헤더에는 적용하지 않습니다.

## 안정성 배지 의미
- `[Stable]`: 호환성 보장 대상
- `[Transitional]`: 안정화 중이며 변경 가능
- `[Internal]`: 호환성 보장 없음

## 호환성 파괴 변경 규칙 (`[Stable]`)
- 파괴적 변경:
  - 공개 타입/함수/상수 제거 또는 이름 변경
  - 함수 시그니처나 콜백 계약 변경
  - 소유권/수명주기 의미를 비호환 방식으로 변경
  - 기존 유효 입력을 거부하도록 동작을 강화
- 비파괴 변경:
  - 안전한 기본값을 가진 새 오버로드/선택 필드 추가
  - 기존 값을 바꾸지 않는 새 상수 추가
  - 문서 보완만 수행한 변경

## 사용 중단(Deprecated) 정책
- 제거 전에 문서에 사용 중단 API를 먼저 명시합니다.
- 사용 중단 API는 최소 1개 릴리스 주기 동안 유지합니다.
- 제거 시 `docs/core-api/migration-note-template.md` 형식의 마이그레이션 노트가 필요합니다.

## 안정(Stable) API 변경 시 PR 요구사항
- `docs/core-api/` 하위 도메인 문서를 함께 갱신합니다.
- 파괴적 변경은 마이그레이션 노트를 추가/갱신합니다.
- API 스모크 소비자 빌드와 CI 검증을 통과합니다.
- `core/include/server/core/api/version.hpp`를 갱신합니다.
- `docs/core-api/compatibility-matrix.json`를 갱신합니다.

## 패키지 버전 연동 규칙
- 패키지 버전의 단일 기준은 `core/include/server/core/api/version.hpp`(`version_string()`)입니다.
- `core/CMakeLists.txt`는 해당 헤더에서 `SERVER_CORE_PACKAGE_VERSION`을 생성하고 `server_coreConfigVersion.cmake`를 출력합니다.
- `server_coreConfigVersion.cmake`는 `SameMajorVersion` 정책을 사용하며, 메이저 버전 불일치는 `find_package(server_core CONFIG)` 단계에서 거부됩니다.
- 버전 증가 가이드:
  - `Stable` API 파괴 변경: 메이저 증가
  - 하위 호환 `Stable` API 추가: 마이너 증가
  - 문서 전용 또는 내부 전용 변경: 패키지 배포가 필요할 때만 패치 증가
