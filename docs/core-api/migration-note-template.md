# 코어(Core) API 마이그레이션 노트 템플릿

## 요약
- 변경 내용:
- 영향받는 API:
- 안정성 레벨:

## 배경
- 해결하려는 문제:
- 기존 API가 부족했던 이유:

## 파괴적 변경 범위
- 제거/이름 변경된 심볼:
- 시그니처 변경:
- 동작 변경:

## 마이그레이션 절차
1. include 갱신:
2. 호출부 갱신:
3. 동작 검증:

## 변경 전
```cpp
// 변경 전 사용 예시
```

## 변경 후
```cpp
// 변경 후 사용 예시
```

## 검증
- 빌드/테스트 근거:
- 런타임/메트릭 검증:

## 사용 중단 공지 템플릿
`Stable` API를 즉시 제거하지 않고 사용 중단 처리할 때 사용합니다.

```md
### 사용 중단 공지
- Deprecated API: `<symbol or header>`
- Replacement: `<symbol or header>`
- First deprecated in: `<version>`
- Planned removal: `<version or release window>`
- Migration note: `docs/core-api/<file>.md`
```

## 릴리스 노트 항목 형식
`docs/core-api/changelog.md`에 아래 형식을 사용합니다.

```md
### 변경됨(Changed)
- `<short behavior/API summary>`

### 파괴적 변경(Breaking)
- `<breaking change summary>`
- Migration: `docs/core-api/<migration-note>.md`

### 사용 중단(Deprecated)
- `<deprecated API and replacement>`
```
