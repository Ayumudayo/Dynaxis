# 공통 빌드

Windows/MSVC와 Linux/GCC는 같은 C++20 소스, Conan 2 recipe/lockfile과 CMake를 사용한다. ImGui/GLFW/OpenGL 클라이언트도 같은 소스로 빌드한다. OS compiler·graphics SDK·driver·시스템 폰트는 플랫폼 준비 사항이다.

## 준비

Git submodule `external/imgui`를 초기화한다. Python 3.10 이상과 CMake 3.23 이상이 필요하다.

```sh
git submodule update --init --recursive
python -m pip install -r conan/requirements.txt
python scripts/build.py --profile full --config Debug --step deps
python scripts/build.py --profile full --config Debug --step configure
python scripts/build.py --profile full --config Debug --step build
python scripts/build.py --profile full --config Debug --step test
```

`--step all`은 deps → configure → build를 묶는다. recipe/lock/profile/Conan 버전/캐시 위치와 build configuration이 같으면 의존성 설치를 생략한다. 의존성을 의도적으로 다시 설치할 때만 `--step deps --refresh`를 쓴다. `--step build`는 설치나 네트워크 접근을 하지 않는다. Conan 실행 파일이 PATH에 없어도 동일 Python의 Conan CLI를 호출한다.

## 구성

| profile | 내용 |
|---|---|
| full | 서버·게이트웨이·워커·도구·ImGui·검사 |
| server | 서버 스택과 검사, GUI 제외 |
| client | ImGui 클라이언트와 공통 코어 |
| core | 최소 TCP 코어와 독립 검사 |

산출물은 `build/<windows|linux|macos>/<profile>/<debug|release>`에 있다. configuration은 디렉터리로 격리한다. Conan 생성 파일은 각 폴더의 `conan/`, 실 패키지는 Conan 공용 캐시에 둔다. `CONAN_HOME`으로 CI 캐시 경로를 지정할 수 있다. `--build-dir`로 저장소 내부 경로를 지정할 수 있다.

기본 Windows compiler profile은 MSVC 194/x64, Linux는 GCC 13/x64다. Linux 준비 환경은 Ubuntu 24.04이며 필요한 graphics SDK는 `Dockerfile.base`를 기준으로 한다. macOS profile은 AppleClang 17/native arch를 제공하지만 현재 검증 플랫폼은 Windows와 Linux다.

`CMakePresets.json`은 동일한 경로와 옵션을 제공한다. 해당 profile의 `--step deps` 이후 `cmake --preset windows-full-debug`, `cmake --build --preset windows-full-debug`처럼 사용할 수 있다. 기존 PowerShell `build.ps1`과 shell `build.sh`는 공통 Python 경로를 사용한다. shell 인자는 Python과 동일하다.

## 의존성 갱신

버전과 recipe revision의 단일 기준은 `conan.lock`이다. direct dependency version은 `conanfile.py`에 명시한다. 새 OS/profile에 필요한 transitive dependency가 있으면 해당 profile로 `conan lock create`를 실행하고 Conan의 `lock merge`로 공통 lock에 합친다. 일반 빌드에서는 partial lock이나 자동 lock 갱신을 허용하지 않는다.

`build/.../conan/graph.json`은 실제 선택된 dependency version/revision과 설정을 기록한다. Windows와 Linux는 플랫폼 시스템 패키지와 binary ID가 다를 수 있지만 공통 C++ library version/revision을 공유한다.

## Docker

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

base image는 compiler·Conan·OS SDK만 설치한다. 앱과 C++ dependency는 공통 recipe로 빌드한다. BuildKit의 `dynaxis-conan2` 캐시에 다운로드/빌드를 보존한다. runtime image는 Conan에서 생성한 runtime library만 가져오며 Ubuntu C++ library 버전으로 바꾸지 않는다. 기존 구형 base를 재사용하려고 `-NoBase`를 지정하지 말고 최초 한 번 새 base를 빌드한다.

Linux 전체 서버/클라이언트/installed consumer 검사는 `docker build --target desktop-build -t dynaxis-desktop-verify .`로 실행한다. 이 stage는 Xvfb/software OpenGL로 클라이언트 10-frame 렌더링 smoke도 실행한다.

## 폰트

`DYNAXIS_FONT_PATH`로 CJK 지원 TTF/TTC를 지정할 수 있다. 기본 후보는 Windows 맑은 고딕, Linux Noto Sans CJK, macOS Apple SD Gothic Neo다. 폰트가 없으면 ImGui 기본 폰트를 사용하므로 한글 표시에는 CJK 폰트가 필요하다.
