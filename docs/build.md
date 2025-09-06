# 빌드 가이드

## 전제
- C++20 컴파일러(MSVC 19.3x+, GCC 11+, Clang 14+)
- CMake 3.20+
- Boost 1.78+ (권장: 1.89)
- Python 3 (선택: opcode 헤더 자동 생성)

## Windows(MSVC) + 수동 설치 Boost
- Boost가 `C:\local\boost_1_89_0`에 설치되어 있다고 가정.
- vcpkg는 사용하지 않음.

### 구성/빌드 명령
PowerShell에서:
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DBOOST_ROOT=C:/local/boost_1_89_0 `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build build --config RelWithDebInfo -j
```

### 비고
- 레포의 `CMakeLists.txt`는 Windows에서 `BOOST_ROOT`가 미설정이면 기본값으로 `C:/local/boost_1_89_0`을 사용하도록 설정되어 있습니다. 다른 경로라면 `-DBOOST_ROOT=...`를 지정하세요.
- `server_core`는 WinSock 종속성(`ws2_32`, `mswsock`)을 자동으로 링크합니다.
- Python 3가 있으면 `protocol/opcodes.json`으로부터 `core/include/server/core/protocol.hpp`가 빌드 시 자동 생성됩니다.

## Linux/WSL
```
sudo apt-get update && sudo apt-get install -y build-essential cmake libboost-system-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

## VSCode 설정(자동 완성/오류 제거)
- 확장 설치: CMake Tools, C/C++(ms-vscode.cpptools)
- 레포에는 VSCode 프리셋이 포함되어 있습니다:
  - `.vscode/settings.json`: CMake Tools를 IntelliSense 공급자로 설정, C++20 지정, Ninja 제너레이터와 `build-vscode` 빌드 디렉터리 사용.
  - `.vscode/c_cpp_properties.json`: `configurationProvider`를 `ms-vscode.cmake-tools`로 지정.
  - `.vscode/extensions.json`: 권장 확장 목록.
- 절차
  1) VSCode로 레포를 열면 CMake가 자동 구성됩니다(처음엔 킷 선택 필요: MSVC/Clang 등).
  2) 하단 `CMake: [Debug/RelWithDebInfo]` 옆 톱니에서 Configure 실행.
  3) 구성 완료 후 IntelliSense가 CMake의 컴파일 옵션/정의/포함경로(Boost 포함)를 인식하므로 Asio 관련 빨간줄이 사라집니다.
  4) 만약 Boost가 기본 경로가 아니라면, `-DBOOST_ROOT=...`를 CMake 옵션에 추가하고 재구성하세요(CMake Tools 상태바의 Configure Settings 사용). 필요 시 `.vscode/settings.json`의 `includePath` 주석 라인을 본인 환경에 맞게 활성화/수정.

참고: 루트 `CMakeLists.txt`는 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`을 설정합니다. Ninja/Makefile 제너레이터 사용 시 `compile_commands.json`이 생성되어 clangd 등의 다른 인텔리센스에도 활용할 수 있습니다.

## 산출물
- `server_app` (빌드 확인용 placeholder)
- `server_core` (라이브러리)

## 스크립트 사용(권장)
- Windows PowerShell:
  - 부스트 빌드/환경변수: `scripts/bootstrap_boost.ps1 -BoostRoot C:/local/boost_1_89_0`
  - 구성+빌드(모든 타깃): `scripts/build.ps1 -Config RelWithDebInfo`
  - 특정 타깃만 빌드: `scripts/build.ps1 -Target server_app` 또는 `-Target dev_chat_cli`
  - 서버 실행: `scripts/build.ps1 -Run server -Port 5000`
  - 클라이언트 실행: `scripts/build.ps1 -Run client -Port 5000`
- Linux/WSL Bash:
  - 구성+빌드(모든 타깃): `scripts/build.sh -r all`
  - 특정 타깃만 빌드: `scripts/build.sh -r build -t server_app`
  - 서버 실행: `scripts/build.sh -r run-server -P 5000`
  - 클라이언트 실행: `scripts/build.sh -r run-client -P 5000`

## 설치(옵션)
공유 사용을 위해 코어를 설치할 수 있습니다.
```
cmake --install build --config RelWithDebInfo --prefix "C:/server-core-sdk"  # Windows 예시
```
- 설치 후 포함 경로: `C:/server-core-sdk/include/server/core/...`
- 라이브러리 경로: `C:/server-core-sdk/lib`

## 코드 생성(opcodes)
- 소스 오브 트루스: `protocol/opcodes.json`
- 생성 대상: `core/include/server/core/protocol.hpp`
- 수동 실행: `python tools/gen_opcodes.py protocol/opcodes.json core/include/server/core/protocol.hpp`
