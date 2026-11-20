# Docker 실행

Windows와 Docker 모두 Conan 2의 같은 recipe/lockfile로 C++ 의존성을 준비하고 CMake로 같은 소스를 빌드한다. [공통 빌드](docs/build.md)를 따른다.

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
pwsh scripts/deploy_docker.ps1 -Action down
```

`Dockerfile.base`는 Ubuntu 24.04의 compiler와 OS graphics SDK, 고정된 Conan CLI만 준비한다. C++ dependency는 apt나 별도 git clone으로 설치하지 않는다. `Dockerfile`의 builder는 공통 `scripts/build.py --profile server --config Release`를 사용하고 Conan이 준비한 runtime library를 함께 배포한다.

`-NoBase`는 새 Conan base를 이미 빌드한 경우에 사용한다. BuildKit의 `dynaxis-conan2` cache mount는 각 서비스 이미지에서 공유한다. 캐시는 재사용되며 `--no-cache`만으로 의존성 버전이 변경되지는 않는다.

```sh
docker build --target desktop-build -t dynaxis-desktop-verify .
```

별도 검증 stage는 같은 recipe와 source로 서버·클라이언트를 빌드하고 단위/설치 검사를 실행한다. Linux ImGui는 Xvfb/software OpenGL의 실제 렌더링 smoke를 통과해야 한다. GUI를 runtime 서버 이미지에 넣지는 않는다.

구성·운영 계약은 [stack README](docker/stack/README.md), 인증은 [signed chat](docs/ops/authenticated-chat.md)에 있다.
