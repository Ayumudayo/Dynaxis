# 빠른 시작(server_core)

## 목표
`[Stable]` `server_core` API만 사용해 최소 실행 바이너리를 컴파일합니다.

이 문서는 package consumption entrypoint입니다. 공개 surface 범위는 `docs/core-api/overview.md`와 `docs/core-api-boundary.md`, 호환성 규칙은 `docs/core-api/compatibility-policy.md`, release/verification command는 `docs/core-api/checklists.md`와 `docs/tests.md`를 기준으로 합니다.

## 최소 예제

```cpp
#include <boost/asio/io_context.hpp>

#include "server/core/app/engine_builder.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/net/hive.hpp"

int main() {
    boost::asio::io_context io;
    server::core::net::Hive hive(io);

    auto runtime = server::core::app::EngineBuilder("quickstart")
        .declare_dependency("sample")
        .build();
    runtime.set_dependency_ok("sample", true);
    runtime.mark_running();

    server::core::concurrent::TaskScheduler scheduler;
    scheduler.post([] {});
    (void)scheduler.poll();

    runtime.mark_stopped();
    return 0;
}
```

## 빌드

```powershell
pwsh scripts/build.ps1 -Config Debug -Target core_public_api_smoke
```

독립 엔진 프로파일(core-only 그래프):

```powershell
cmake --preset windows-core-engine
cmake --build --preset windows-core-engine-debug --target server_core
```

## 외부 CMake 소비자 (`find_package`)

`server_core`는 CMake 패키지 파일과 exported target을 설치합니다.

```cmake
find_package(server_core CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE server_core::server_core)
```

### 로컬 Windows 스모크 (레포 컨텍스트)

```powershell
pwsh scripts/build.ps1 -Config Debug -Target server_core
cmake --install build-windows --config Debug --prefix build-windows/install-smoke
```

소비자 configure/build (의존성 prefix 포함):

```powershell
cmake -S build-windows/package-smoke -B build-windows/package-smoke/build --fresh `
  -DCMAKE_PREFIX_PATH="${PWD}/build-windows/install-smoke" `
  -DCMAKE_TOOLCHAIN_FILE="${PWD}/build-windows/conan/build/generators/conan_toolchain.cmake"
cmake --build build-windows/package-smoke/build --config Debug
```

의존성 탐색에 실패하면 현재 build tree의 Conan generator 출력과 install prefix가 모두 존재하는지 확인합니다.

자동화된 계약 검증에서는 같은 흐름을 `CoreInstalledPackageConsumer` 테스트로 실행합니다. 이 테스트는 install prefix를 만든 뒤 별도 consumer configure/build를 수행하고, `server_core_installed_consumer`와 `server_core_extensibility_consumer` 실행까지 확인합니다.

```powershell
ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure
```

### 로컬 Linux package parity smoke (Docker-backed)

현재 저장소는 Linux package parity를 `dynaxis-base:latest` 기반 컨테이너에서 repeatable하게 검증합니다.

```powershell
pwsh scripts/run_linux_installed_consumer.ps1
```

이 스크립트는 Linux container 안에서:

- `server_core`와 public-api smoke targets를 빌드하고
- installed prefix를 만든 뒤
- `CoreInstalledPackageConsumer`, `CoreApiBoundaryFixtures`, `CoreApiStableGovernanceFixtures`를 실행합니다.

## 참고
- 이 빠른 시작 문서는 의도적으로 `Transitional`, `Internal` 헤더를 사용하지 않습니다.
- 공개 API 경계는 `docs/core-api-boundary.md`에서 정의합니다.
