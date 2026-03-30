# Build / Test Ownership

현재 저장소의 build/test ownership은 아래 기준을 따른다.

## Build Ownership

- root `CMakeLists.txt`
  - 옵션 선언
  - generated public header configure 단계
  - `proto/`, `core/`, `infra/`, `server/`, `gateway/`, `tools/`, `tests/` 진입
- `tools/CMakeLists.txt`
  - `wb_worker`, `wb_emit`, `wb_check`, `wb_dlq_replayer`
  - `admin_app`
  - `migrations_runner`
  - `stack_loadgen`

## Test Ownership

- `tests/CMakeLists.txt`
  - shared test prelude
  - GTest discovery helper
  - domain manifest dispatch
- `tests/core/CMakeLists.txt`
  - core behavior/fuzz/plugin runtime targets
- `tests/gateway/CMakeLists.txt`
  - gateway/shared-state targets
- `tests/server/CMakeLists.txt`
  - chat/runtime/plugin-chain targets
- `tests/contracts/CMakeLists.txt`
  - public API/package consumer/docs-policy contract lane
- `tests/policy/CMakeLists.txt`
  - stack/kubernetes/python policy and operational proof lane

## Intent

- 폴더 구조를 보면 build/test ownership이 바로 읽혀야 한다.
- 파일 경로를 대규모로 옮기기 전에도 manifest ownership부터 domain 기준으로 정렬한다.
- contract/public API fixture 경로는 현재 도구와 검증 스크립트가 기대하는 위치를 유지한다.
