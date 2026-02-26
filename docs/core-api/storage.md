# 스토리지(Storage) API 가이드

## 안정성
- 이 모듈은 현재 `[Stable]` 공개 헤더를 제공하지 않습니다.
- 저장소 계약은 `[Internal]`로 분류되며 호환성 보장 없이 변경될 수 있습니다.

## 현재 계약 형태
- 내부 계약은 repository 인터페이스, `IUnitOfWork`, `IConnectionPool`, `DbWorkerPool`를 포함합니다.
- repository DTO와 인터페이스 집합은 채팅 도메인(`user/room/message/membership/session`)에 종속되어 있습니다.
- 공개 엔진 소비자는 저장소 내부 구현에 직접 의존하지 않아야 합니다.

## 사용 규칙
- repository 호출은 `IUnitOfWork`의 commit/rollback 경계 내부에서 수행합니다.
- 비동기 DB 실행은 앱/서비스 어댑터 뒤로 숨깁니다.
- 범용 엔진 중립 SPI가 도입되기 전까지 저장소 심볼은 모두 내부 전용으로 취급합니다.
