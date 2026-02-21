# 게이트웨이(Gateway) 확장 인터페이스 후보

## 목적
- 이번 단계에서 런타임 확장 코드를 추가하지 않고 gateway 측 확장 지점의 설계 목표를 정의합니다.
- gateway 훅 표면을 승격하기 전에 호환성 경계를 명시적으로 고정합니다.

## 훅 표면 후보
- **라우팅 정책 훅(Routing policy hook)**: 프런트 세션 메타데이터와 backend 후보 집합을 받아 선택된 backend id를 반환
- **접속 허용 정책 훅(Admission policy hook)**: 연결 메타데이터를 받아 allow/deny와 사유 코드를 반환
- **세션 수명주기 훅(Session lifecycle hook)**: connect/disconnect/reconnect 이벤트를 받아 관측/정책 부작용을 처리

## 수명주기 계약
- 훅 초기화는 gateway bootstrap 중 listener accept 루프 시작 전에 수행됩니다.
- 훅 콜백은 gateway 요청 경로에서 실행되며 논블로킹이어야 합니다.
- 훅 종료는 graceful stop 중 backend 연결 teardown 완료 전에 수행됩니다.

## 오류 모델
- 훅 로드/초기화 실패: 명시적 오류 로그와 함께 gateway 시작을 즉시 실패 처리합니다.
- 훅 콜백 실패: gateway는 내장 기본 정책으로 폴백하고 실패 메트릭을 증가시킵니다.
- 훅 타임아웃: 콜백 실패로 간주하며 기본 정책 경로를 사용합니다.

## 호환성 표면
- Stable 후보 인터페이스는 다음을 정의해야 합니다.
  - 불변 입력 DTO 형태
  - 결정적 콜백 반환 스키마
  - 명시적 timeout/deadline 동작
- 파괴적 변경에는 마이그레이션 노트와 호환성 분류 갱신이 필요합니다.

## 이번 단계 비목표
- 새 gateway 플러그인 로더 구현 없음
- 동적 코드 로딩 정책 결정 없음
