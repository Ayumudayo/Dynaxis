# 빌드 가이드

## 전제
- C++20 컴파일러(MSVC 19.3x+, GCC 11+, Clang 14+)
- CMake 3.20+
- Boost 1.78+ (권장: 1.89)
- Python 3 (선택: opcode 헤더 자동 생성)
- vcpkg(권장): FTXUI 및 의존성 설치에 사용. 매니페스트 `vcpkg.json` 제공.

## 권장: vcpkg 매니페스트 빌드(FTXUI 포함)
- 의존성: `vcpkg.json`에 `ftxui`가 선언되어 있습니다.
- 환경 변수 `VCPKG_ROOT`를 vcpkg 루트로 설정(예: `C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\vcpkg`).
- 기본 트리플릿: Windows `x64-windows`, Linux `x64-linux`.

### PowerShell(Windows)
- 자동 구성/빌드/실행(서버+클라):
  - `scripts/build.ps1 -UseVcpkg -Run both`
- 서버만 실행: `scripts/build.ps1 -UseVcpkg -Run server -Port 5000`
- 클라만 실행: `scripts/build.ps1 -UseVcpkg -Run client -Port 5000`
- 메모
  - 스크립트는 `vcpkg.json`을 감지하면 toolchain을 자동 지정하고, 필요 시 `builtin-baseline`을 주입한 뒤 `vcpkg install`을 실행합니다.
  - MSVC+vcpkg 환경에서 런타임 불일치를 피하기 위해 `RelWithDebInfo` 요청 시 자동으로 `Debug` 구성으로 빌드합니다.

### Linux/WSL
```
sudo apt-get update && sudo apt-get install -y build-essential cmake
# vcpkg 설치 후 VCPKG_ROOT 지정
export VCPKG_ROOT=$HOME/vcpkg
pwsh scripts/build.ps1 -UseVcpkg -Config Debug
```

## 대안: 수동 Boost 경로 지정(비권장)
- Boost가 `C:\\local\\boost_1_89_0`에 있다고 가정(다르면 `-DBOOST_ROOT=...`).
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBOOST_ROOT=C:/local/boost_1_89_0 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo -j
```

## VSCode 설정(요약)
- 확장: CMake Tools, C/C++(ms-vscode.cpptools)
- 절차
  - 킷 선택 후 Configure → Build.
  - vcpkg 사용 시 의존성/포함 경로가 자동 반영됩니다.

참고: 루트 `CMakeLists.txt`는 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`을 설정합니다.

## 산출물/실행
- 바이너리
  - `server_app`: 테스트용 서버
  - `dev_chat_cli`: FTXUI 기반 CLI 클라이언트
  - `wb_worker`: Redis Streams → Postgres 배치 커밋 워커
  - `wb_dlq_replayer`: DLQ 재처리 도구(옵션)
  - `wb_emit`/`wb_check`: 스모크 테스트 보조 도구
- 실행 기본값
  - 서버: 기본 포트 `5000`
  - 클라: 인자 생략 시 `127.0.0.1:5000` 접속
- 스크립트 실행 예
  - `scripts/build.ps1 -UseVcpkg -Run both` → 서버 백그라운드 실행 후 클라 시작
  - 개별 실행은 `-Run server | client` 활용

## 빠른 시작(요약)
- 환경 파일: 루트 `.env`에 DB_URI/REDIS_URI 등 설정(예시는 `docs/getting-started.md` 참조)
- 서버 실행: `build-msvc/server/Debug/server_app.exe 5000`
- 워커 실행: `build-msvc/Debug/wb_worker.exe`
- 스모크: `scripts/smoke_wb.ps1`
- 메트릭: `METRICS_PORT` 설정 시 `curl http://127.0.0.1:9090/metrics`

## 설치(옵션)
```
cmake --install build-msvc --config Debug --prefix "C:/server-core-sdk"  # Windows 예시
```
- 설치 후 포함 경로: `C:/server-core-sdk/include/server/core/...`
- 라이브러리 경로: `C:/server-core-sdk/lib`

## 코드 생성(opcodes)
- 소스 오브 트루스: `protocol/opcodes.json`
 - 생성 대상: `core/include/server/core/protocol/opcodes.hpp`
 - 수동 실행: `python tools/gen_opcodes.py protocol/opcodes.json core/include/server/core/protocol/opcodes.hpp`
