# 리네임 경계

Dynaxis 저장소는 기존 프로젝트명 토큰을 전수 제거하고, 새 브랜드는 사람과 배포 artifact가 보는 표면에만 적용한다. 반대로 코드 내부, 빌드 식별자, 테스트 harness는 역할 기반 이름으로 정리하고, legacy 토큰은 이 문서에만 기록한다.

이 문서가 필요한 이유는 리브랜딩이 단순 치환 작업이 아니기 때문이다. 브랜드 이름을 모든 식별자에 그대로 밀어 넣으면 재사용성은 떨어지고, 반대로 사용자 노출 표면까지 generic 이름으로 남기면 제품 정체성이 흐려진다. 따라서 어디까지 브랜드를 적용하고 어디서는 역할 이름을 유지할지 경계를 정해야 한다.

## Dynaxis로 즉시 교체할 표면

다음 표면은 제품명 또는 배포명에 해당하므로 `Dynaxis` 기준으로 즉시 교체한다.

- 사용자와 운영자에게 노출되는 텍스트: 루트 README, 서브 README, 관리자 UI, 클라이언트 창 제목, crash handler 문자열
- 배포 artifact: Conan package `dynaxis`, Docker 이미지 `dynaxis-base`, `dynaxis-server`, `dynaxis-gateway`, `dynaxis-worker`, `dynaxis-admin`, `dynaxis-migrator`
- compose/stack 식별자: project, network, volume 이름인 `dynaxis-stack`, `dynaxis_stack_postgres_data`, `dynaxis_stack_redis_data`
- 기본 예시 값: Postgres `dynaxis`, `dynaxis_db`, Redis channel prefix `dynaxis:`
- 운영 문서와 CI 예시: 이미지명, 컨테이너명, compose project name, loadgen network 예시

대표 경로:

- `README.md`, `docs/README.md`, `docs/build.md`, `docs/configuration.md`, `docs/core-design.md`
- `tools/admin_app/admin_ui.html`, `tools/admin_app/main.cpp`, `client_gui/src/gui_manager.cpp`, `client_gui/src/main.cpp`
- `Dockerfile`, `Dockerfile.base`, `docker/stack/docker-compose.yml`, `scripts/deploy_docker.ps1`, `.github/workflows/*.yml`

이 표면을 즉시 바꾸는 이유는 사용자가 실제로 보는 이름과 배포 이름이 여기에 있기 때문이다. 이 계층에서 이름이 섞이면 운영과 문서, 빌드 결과가 서로 다른 제품처럼 보일 수 있다.

## generic 또는 역할 기반 이름으로 바꿀 표면

다음 표면은 새 브랜드 이름으로 치환하지 않고, 역할 기반 이름으로 정리한다.

- 빌드 옵션과 캐시 키: `build_profile`, `ENABLE_SANITIZERS`, `ENABLE_IPO`, `LUAJIT_SUBMODULE_DIR`, `SOL2_SUBMODULE_DIR`
- CMake helper와 파일명: `configure_luajit_submodule`, `configure_sol2_submodule`, `luajit_submodule.cmake`, `sol2_submodule.cmake`
- build-info 매크로: `BUILD_GIT_HASH`, `BUILD_GIT_DESCRIBE`, `BUILD_TIME_UTC`
- runtime 관측/설정 계약: `runtime_build_info`, `runtime_dependency_ready`, `runtime_dependencies_ok`, `runtime_lifecycle_phase`, `runtime_lifecycle_phase_code`, `RUNTIME_TRACING_ENABLED`, `RUNTIME_TRACING_SAMPLE_PERCENT`
- 내부 프로세스 공유 env: `SERVER_CORE_SERVICE_REGISTRY`
- 테스트 harness: `tests::plugin`, `TEST_PLUGIN_EXPORT`, `test_plugin_api_v1`, 중립적 temp/cache/script path 접두사
- 샘플 문구: 제품명을 박아 넣지 않고 `welcome to the server` 같은 generic 문구 사용
- 보조 스크립트 경로: machine-local absolute path 대신 repo-relative path 사용

대표 경로:

- `conanfile.py`, `CMakeLists.txt`, `core/CMakeLists.txt`, `core/include/server/core/build_info.hpp`, `core/src/trace/context.cpp`, `core/src/util/service_registry.cpp`
- `tests/core/*`, `tests/server/*`, `tests/python/run_stack_test.py`
- `tools/reorganize_dashboard.py`, `server/scripts/on_login_welcome.lua`, `docker/stack/scripts/on_login_welcome.lua`

이 표면에서 역할 기반 이름을 유지하는 이유는 브랜드보다 의미가 더 중요하기 때문이다. 빌드 플래그, runtime contract, 테스트 harness는 재사용성과 해석 가능성이 우선이고, 제품명은 오히려 잡음을 늘릴 수 있다.

## 그대로 유지할 표면

다음 표면은 이미 역할 중심 어휘이므로 유지한다.

- 실행 파일과 라이브러리 이름: `server_app`, `gateway_app`, `wb_worker`, `server_core`
- 네임스페이스와 역할 접두: `server::core`, `GATEWAY_*`, `SERVER_*`, `WB_*`, `MSG_*`, `ERR_*`
- 데이터와 도메인 이름: `session_events`, `gateway/instances/`, `gateway/session/`
- C++ 상수와 enum 관례: `k*`

이 표면을 억지로 바꾸지 않는 이유는 이미 브랜드와 무관하게 역할을 잘 설명하고 있기 때문이다. 잘 설명하는 이름을 다시 바꾸면 비용만 늘고 이득이 없다.

## legacy를 기록만 하고 남길 곳

아래 legacy 토큰은 이 문서에만 기록을 허용한다.

- `Knights`
- `knights`
- `KNIGHTS_`
- `knights::`
- `knights-`
- `knights:`

허용 규칙:

- tracked 파일에서 legacy 토큰은 이 문서를 제외하고 남아 있으면 안 된다.
- 새 호환 alias를 도입해야 할 특별한 사유가 생기면, 먼저 이 문서에 이유와 종료 기준을 기록한다.

이 제한을 두는 이유는 legacy alias가 한 번 들어오면 생각보다 오래 남기 쉽기 때문이다. 기록과 허용 경계를 한 문서에 묶어 두면, 예외가 생겨도 나중에 정리하기가 쉽다.
