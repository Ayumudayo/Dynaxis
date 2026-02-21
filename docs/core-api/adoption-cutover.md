# 코어 API 채택 및 컷오버 계획

## 목표
- `server`, `gateway`, `tools`가 `core` 공개 계약을 사용할 때 `Stable` 헤더만 소비하도록 전환합니다.
- `Transitional` 헤더 개수를 0으로 유지합니다.

## 현재 점검 스냅샷

### 과도기(Transitional) 헤더
- `docs/core-api-boundary.md` 기준 현재 `Transitional` 행은 0개입니다.

### 최상위 모듈별 Internal 헤더 사용 현황
- `server`는 일부 구현 경로에서만 internal 헤더를 포함합니다.
  - `server/src/app/core_internal_adapter.cpp` -> internal accept-loop/network/runtime-state/storage worker/crash-hook 계약(어댑터 경계)
  - `server/src/storage/postgres/connection_pool.cpp` -> internal storage repository/transaction 계약
  - `server/src/chat/chat_service_core.cpp` -> internal storage connection-pool 계약
  - `server/src/chat/handlers_login.cpp` -> internal storage connection-pool 계약
  - `server/src/chat/handlers_join.cpp` -> internal storage connection-pool 계약
  - `server/src/chat/handlers_chat.cpp` -> internal storage connection-pool 계약
  - `server/src/chat/handlers_leave.cpp` -> internal storage connection-pool 계약
- `gateway` internal-header include 적중: 없음(현재 grep 점검 기준)
- `tools` internal-header include 적중: 없음(현재 grep 점검 기준)

## 컷오버 단계

### 단계 A(Phase A) - Server 네트워크 경계 어댑터
- internal session/runtime-state 사용을 server 로컬 어댑터 뒤로 숨깁니다.
- internal include 직접 참조를 어댑터 구현 단위로 제한합니다.
- 외부로 노출되는 server 모듈 경계는 `Stable` 계약으로 유지합니다.
- 상태: 시작됨
  - `server/include/server/app/core_internal_adapter.hpp`, `server/src/app/core_internal_adapter.cpp` 추가
  - `server/src/app/bootstrap.cpp`는 크래시 핸들러 설치, 런타임 연결 수 조회, 세션 리스너 start/stop, DB 풀/worker 수명주기 처리 시 어댑터 API를 사용
  - `server/src/app/router.cpp`는 session 헤더 직접 include를 제거하고 `ChatService::NetSession` 별칭 사용
  - `server/include/server/storage/postgres/connection_pool.hpp`는 internal storage 헤더 직접 include 대신 forward declaration 사용

### 단계 B(Phase B) - Server 저장소 경계 어댑터
- 채팅 도메인 저장소 결합을 server 로컬 인터페이스 뒤로 이동합니다.
- `core` 저장소 헤더는 internal 범위를 유지하고, server 저장소 구현 바깥으로 include가 퍼지지 않게 합니다.
- 상태: 진행 중
  - 채팅 핸들러와 `chat_service_core`에서 `repositories.hpp`/`unit_of_work.hpp` 직접 include를 제거했고, 저장소 API 사용은 `connection_pool.hpp` 경유로 통합했습니다.

### 단계 C(Phase C) - 강제 및 회귀 방지
- 공개 예제/소비자 테스트에 `Stable` 헤더 전용 include 정책을 강제합니다.
- CI에서 boundary 및 stable-governance fixture 검증을 유지합니다.

## 검증 기록
- Boundary 계약 점검: `python tools/check_core_api_contracts.py --check-boundary`
- Boundary fixture 점검: `python tools/check_core_api_contracts.py --check-boundary-fixtures`
- Stable governance fixture 점검: `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`
- 소비자 테스트: `ctest --preset windows-test -R "CorePublicApi|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure`

## 종료 기준
- `docs/core-api-boundary.md`의 `Transitional = 0` 상태를 유지합니다.
- `gateway`, `tools`는 internal `core` 헤더 include가 없는 상태를 유지합니다.
- `server` internal include는 구현 어댑터 내부에 한정되고 공개/예제 계약으로 전파되지 않습니다.
