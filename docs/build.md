# 빌드 가이드

Dynaxis의 표준 런타임/검증 환경은 Linux(Docker) 풀스택(`docker/stack`)이다.
Windows는 개발/디버깅(빌드/클라이언트) 용도로 유지한다.

## 전제
- CMake 3.21+ (`CMakePresets.json` 사용)
- C++20 컴파일러
- Conan 2 (`scripts/build.ps1`는 Conan-generated toolchain만 지원)
- Python 3 (선택: opcodes/wire codegen 자동 실행)

## 로컬 사전 검증

큰 변경(빌드/의존성/워크플로우/다중 모듈 변경)은 push 전에 로컬 검증을 통과하는 것을 권장한다.

- 핵심 원칙: 로컬 검증 실패 상태에서는 push하지 않는다.
- 권장 최소 검증: `pwsh scripts/build.ps1 -Config Release`, `ctest --preset windows-test --output-on-failure`

## 윈도우(Windows) 개발 빌드

### 1) 빌드
```powershell
# 권장: 경량 클라이언트 전용 릴리즈 빌드
pwsh scripts/build.ps1 -ClientOnly -Target client_gui

# 서버/테스트까지 포함한 전체 Windows 개발 빌드(필요할 때만)
pwsh scripts/build.ps1 -Config Debug

# 단일 타깃 빌드(서버 예시)
pwsh scripts/build.ps1 -Config Debug -Target server_app
```

`scripts/build.ps1`는 Conan 출력/toolchain을 자동으로 사용한다. `conan` 실행 파일이 보이지 않으면 먼저 Conan 2 설치 상태를 확인한다.

### 2) 테스트
```powershell
ctest --preset windows-test
```

### 3) 산출물 위치(예시)
- `build-windows-client/client_gui/Release/client_gui.exe` (권장 client-only 빌드)
- `build-windows/server/Debug/server_app.exe` (전체 Windows 빌드 시)
- `build-windows/gateway/Debug/gateway_app.exe` (전체 Windows 빌드 시)
- `build-windows/Debug/wb_worker.exe` (전체 Windows 빌드 시)

## Factory Package Install Smoke

`server_storage_pg_factory`와 `server_storage_redis_factory`는 install/export/package-consumer smoke를 지원한다.
첫 단계의 package-first extraction surface이며, app-local helper target은 여전히 패키지 대상이 아니다.

```powershell
cmake --install build-windows --config Debug --prefix build-windows/install-smoke
```

Postgres factory consumer 예시:

```powershell
cmake -S tests/package/factory_pg_consumer -B build-windows/factory-pg-consumer/build --fresh `
  -DCMAKE_PREFIX_PATH="${PWD}/build-windows/install-smoke" `
  -DCMAKE_TOOLCHAIN_FILE="${PWD}/build-windows/conan/build/generators/conan_toolchain.cmake"
cmake --build build-windows/factory-pg-consumer/build --config Debug
```

Redis factory consumer 예시:

```powershell
cmake -S tests/package/factory_redis_consumer -B build-windows/factory-redis-consumer/build --fresh `
  -DCMAKE_PREFIX_PATH="${PWD}/build-windows/install-smoke" `
  -DCMAKE_TOOLCHAIN_FILE="${PWD}/build-windows/conan/build/generators/conan_toolchain.cmake"
cmake --build build-windows/factory-redis-consumer/build --config Debug
```

자동화된 검증에서는 `FactoryPgInstalledPackageConsumer`, `FactoryRedisInstalledPackageConsumer` contract test가 같은 흐름을 실행한다.

## Factory Package Publish Bundle

first milestone publish automation은 `server_storage_pg_factory`와 `server_storage_redis_factory`를 별도 package manager publish가 아니라
하나의 Release install-prefix bundle로 배포한다.

로컬 생성:

```powershell
pwsh scripts/publish_factory_packages.ps1 -BuildDir build-windows -OutputRoot artifacts/factory-packages -Zip -CleanOutput
```

출력:
- bundle 디렉터리: `artifacts/factory-packages/dynaxis-factory-packages-windows-x64-release-<version>/`
- staged prefix: `.../prefix/`
- manifest: `.../manifest.json`
- zip: `...zip`

이 bundle에는 다음 package surface가 포함된다.
- `server_core`
- `server_storage_pg_factory`
- `server_storage_redis_factory`

다음 항목은 의도적으로 포함되지 않는다.
- app-local helper target
- compatibility umbrella target의 우선 surface
- `server_state_redis_factory`

CI 수동 artifact 생성:
- GitHub Actions `Factory Package Publish` workflow (`workflow_dispatch`)
- 업로드 artifact root: `artifacts/factory-packages/**`

## 리눅스(Linux) (표준 런타임 = Docker 풀스택)

`scripts/deploy_docker.ps1`를 통해 base 이미지/compose profile/포트 매핑을 일관되게 유지한다.

```powershell
# 스택 기동
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build

# 스택 기동 + 관측성(Observability: Prometheus/Grafana)
pwsh scripts/run_full_stack_observability.ps1

# 스택 종료
pwsh scripts/deploy_docker.ps1 -Action down
```

## 메모리/정의되지 않은 동작 검사 (Sanitizer: ASan/UBSan)

- CMake 옵션: `ENABLE_SANITIZERS=ON`
- 프리셋: `linux-asan`

로컬 Linux 환경에서:
```bash
cmake --preset linux-asan
cmake --build --preset linux-asan --target server_app --parallel
```

CI에서는 `dynaxis-base` 컨테이너 내부에서 `linux-asan` 빌드를 수행한다. (`.github/workflows/ci-hardening.yml` 참고)

## 코드 생성(opcodes)

- 소스(JSON)
  - system(core): `core/protocol/system_opcodes.json`
  - game(server): `server/protocol/game_opcodes.json`
- 생성기
  - 헤더 생성: `tools/gen_opcodes.py`
  - 문서/검증: `tools/gen_opcode_docs.py` → `docs/protocol/opcodes.md`

규칙: 생성된 헤더는 수동으로 편집하지 말고(JSON + generator 수정) 다시 생성한다.
