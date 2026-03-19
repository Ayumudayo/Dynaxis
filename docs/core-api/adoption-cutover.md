# 코어 API 채택 및 컷오버 계획

## 목표
- `server`, `gateway`, `tools`가 `core` 공개 계약을 사용할 때 `Stable` 헤더만 소비하도록 전환합니다.
- `Transitional` 헤더는 현재 문서화된 범위로만 제한하고, 공개 예제/installed-consumer 경로에는 유입되지 않게 유지합니다.

## 현재 점검 스냅샷

### 과도기(Transitional) 헤더
- `docs/core-api-boundary.md` 기준 현재 `Transitional` 헤더는 compatibility wrapper와 service-owned chat extensibility 계층입니다.
  - `server/core/fps/direct_bind.hpp`
  - `server/core/fps/direct_delivery.hpp`
  - `server/core/fps/transport_quality.hpp`
  - `server/core/fps/transport_policy.hpp`
  - `server/core/fps/runtime.hpp`
  - `server/core/mmorpg/migration.hpp`
  - `server/core/mmorpg/topology.hpp`
  - `server/core/mmorpg/world_drain.hpp`
  - `server/core/mmorpg/world_transfer.hpp`
- 현재 package/public-api 예제와 installed-consumer 경로는 reusable core extensibility mechanism을 `Stable` 헤더로 사용하고, chat-specific transitional ABI는 사용하지 않습니다.

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
- 상태: 완료
  - `server/include/server/app/core_internal_adapter.hpp`, `server/src/app/core_internal_adapter.cpp` 추가
  - `server/src/app/bootstrap.cpp`는 크래시 핸들러 설치, 런타임 연결 수 조회, 세션 리스너 start/stop, DB 풀/worker 수명주기 처리 시 어댑터 API를 사용
  - `server/src/app/router.cpp`는 session 헤더 직접 include를 제거하고 `ChatService::NetSession` 별칭 사용
  - `server/include/server/storage/postgres/connection_pool.hpp`는 internal storage 헤더 직접 include 대신 forward declaration 사용

### 단계 B(Phase B) - Server 저장소 경계 어댑터
- 채팅 도메인 저장소 결합을 server 로컬 인터페이스 뒤로 이동합니다.
- `core` 저장소 헤더는 internal 범위를 유지하고, server 저장소 구현 바깥으로 include가 퍼지지 않게 합니다.
- 상태: 완료
  - 채팅 핸들러와 `chat_service_core`에서 `repositories.hpp`/`unit_of_work.hpp` 직접 include를 제거했고, 저장소 API 사용은 `connection_pool.hpp` 경유로 통합했습니다.
  - Postgres/Redis concrete backend는 narrower factory target(`server_storage_pg_factory`, `server_storage_redis_factory`)과 implementation object로 분리했고, 기존 broader target 이름은 compatibility umbrella로 유지했습니다.

### 단계 C(Phase C) - 강제 및 회귀 방지
- 공개 예제/소비자 테스트에 `Stable` 헤더 전용 include 정책을 강제합니다.
- CI에서 boundary 및 stable-governance fixture 검증을 유지합니다.
- 설치된 prefix를 대상으로 `find_package(server_core)` consumer 빌드를 자동 검증합니다.
- 상태: 완료

## 검증 기록
- Boundary 계약 점검: `python tools/check_core_api_contracts.py --check-boundary`
- Boundary fixture 점검: `python tools/check_core_api_contracts.py --check-boundary-fixtures`
- Stable governance fixture 점검: `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`
- 소비자 테스트: `ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure`

## 종료 기준
- `docs/core-api-boundary.md`의 `Transitional` 인벤토리가 실제 현재 public package surface와 일치합니다.
- chat-specific `Transitional` 헤더는 공개 예제/installed-consumer 경로로 전파되지 않습니다.
- `gateway`, `tools`는 internal `core` 헤더 include가 없는 상태를 유지합니다.
- `server` internal include는 구현 어댑터 내부에 한정되고 공개/예제 계약으로 전파되지 않습니다.

## Package-First Follow-Up (2026-03-12)
- `server_storage_redis_factory`와 `server_storage_pg_factory` 도입 이후, helper target의 concrete backend 결합은 narrower factory seam 뒤로 줄어들었습니다.
- 남은 source-level coupling은 의도적으로 app/tool-local composition helper와 adapter 구현 파일에 집중되어 있습니다.
  - `server/src/app/core_internal_adapter.cpp`
  - `server/src/state/redis_backend_factory.cpp`
  - `gateway/src/registry_backend_factory.cpp`
  - `tools/admin_app/redis_client_factory.cpp`
  - `tools/wb_common/redis_client_factory.cpp`
- 이 상태는 installed-package consumer 검증을 막지 않으므로, 현재 시점에서 raw source repo split을 다시 여는 것보다 package-first 경로가 더 안전합니다.
- 첫 packageization milestone에서는 `server_storage_pg_factory`, `server_storage_redis_factory`의 install/export/config scaffolding과 installed-package consumer proof를 우선 확보했습니다.
- 후속 extraction이 필요해지면 아래 순서를 우선합니다.
  1. `server_core`와 narrower backend factory package를 먼저 배포/버전화합니다.
  2. `gateway_backends`, `admin_app_backends`, `server_app_backends`, `wb_common_redis_factory` 같은 app-local helper는 integration repo에 남깁니다.
  3. 둘 이상의 실행 파일이 동일 helper 구현을 공유하게 될 때만 helper 자체의 package 승격 또는 별도 repo 이동을 재평가합니다.
