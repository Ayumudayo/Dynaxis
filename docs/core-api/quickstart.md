# 빠른 시작(server_core)

## 목표
`[Stable]` `server_core` API만 사용해 최소 실행 바이너리를 컴파일합니다.

## 최소 예제

```cpp
#include <boost/asio/io_context.hpp>

#include "server/core/app/app_host.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/net/hive.hpp"

int main() {
    boost::asio::io_context io;
    server::core::net::Hive hive(io);

    server::core::app::AppHost host{"quickstart"};
    host.declare_dependency("sample");
    host.set_dependency_ok("sample", true);
    host.set_ready(true);
    host.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kRunning);

    server::core::concurrent::TaskScheduler scheduler;
    scheduler.post([] {});
    (void)scheduler.poll();

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

자동화된 계약 검증에서는 같은 흐름을 `CoreInstalledPackageConsumer` 테스트로 실행합니다. 관련 label은 `contract;public-api;installed-consumer`이며, Windows multi-config 빌드에서는 다음처럼 `tests` 하위 test-dir로 실행하는 것이 가장 안전합니다.

```powershell
ctest -C Debug --test-dir build-windows/tests -L contract --output-on-failure
```

## 참고
- 이 빠른 시작 문서는 의도적으로 `Transitional`, `Internal` 헤더를 사용하지 않습니다.
- 공개 API 경계는 `docs/core-api-boundary.md`에서 정의합니다.
