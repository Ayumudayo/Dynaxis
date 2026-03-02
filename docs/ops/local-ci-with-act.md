# 로컬 CI 가이드 (act, 제한 모드)

이 문서는 `act`를 "로컬 전체 CI 재현"이 아니라 "워크플로우 문법 + Windows self-hosted 잡 점검" 용도로 제한해 사용하는 표준 절차를 정의합니다.

## 목적
- 워크플로우 문법/조건식/스크립트 오류를 원격 CI 전에 빠르게 발견한다.
- Windows 경로(Conan/MSVC/ctest) 회귀를 push 전에 1차 확인한다.

## 적용 범위 (중요)

`act`로 실행/게이트하는 범위:
- `windows-conan-deps` (best-effort)
- `windows-fast-tests` (기본 게이트)
- `core-api-consumer-windows` (`core/include/server/core/**` 변경 시 추가)

`act` 범위에서 제외하는 잡:
- `core-api-consumer-linux`
- `linux-docker-stack`

위 Linux 잡은 Docker-in-Docker 및 workspace mount 차이로 로컬 `act` 재현성이 낮으므로, `scripts/deploy_docker.ps1` 기반 로컬 검증 + 원격 GitHub CI 결과를 기준으로 판단한다.

## 준비
1. Docker Desktop (Linux containers)
2. `act` 설치
3. 저장소 루트 `.actrc` 구성

```text
-P ubuntu-latest=catthehacker/ubuntu:act-latest
-P windows-latest=-self-hosted
```

Windows self-hosted 잡 실행을 위해 1회 설정:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned -Force"
```

## 기본 확인
```bash
act -W .github/workflows/ci.yml -l
```

## 빠른 Dry-run
```bash
act pull_request -W .github/workflows/ci.yml -j windows-conan-deps --dryrun
act pull_request -W .github/workflows/ci.yml -j windows-fast-tests --dryrun
```

## Push 전 게이트 (큰 변경)
다음 조건 중 하나라도 만족하면 "큰 변경"으로 간주합니다.
- CMake/Conan/워크플로우/빌드 스크립트 변경
- 3개 이상 모듈(`core/`, `server/`, `gateway/`, `tools/`) 동시 변경
- 런타임/의존성/코드 생성 경로 변경

큰 변경은 아래 Windows 로컬 CI를 통과한 뒤 push 합니다.

```bash
act pull_request -W .github/workflows/ci.yml -j windows-fast-tests
```

`core/include/server/core/**`를 건드렸다면 아래도 추가로 실행합니다.

```bash
act pull_request -W .github/workflows/ci.yml -j core-api-consumer-windows
```

필요 시 캐시 액션 호환성을 높이기 위해 아래 옵션을 사용할 수 있습니다.

```powershell
act push -W .github/workflows/ci.yml -j windows-fast-tests --cache-server-path C:\act-cache
```

## Linux 로컬 검증 (act 대체 경로)

Linux/Docker 계열 검증은 아래 경로를 사용합니다.

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
python tests/python/verify_pong.py
python tests/python/test_load_balancing.py
pwsh scripts/deploy_docker.ps1 -Action down
```

## 운영 규칙
- 지원 범위(`windows-*`)의 로컬 CI가 실패한 상태에서는 push하지 않는다.
- 실패 시 로그를 기준으로 수정 후 동일 잡을 재실행한다.
- Linux 잡의 pass/fail 게이트는 원격 GitHub CI를 기준으로 한다.

## 제한 사항
- `act`는 GitHub hosted runner와 100% 동일하지 않다.
- Windows 잡은 `-self-hosted`로 호스트에서 실행되므로 로컬 환경 영향에 주의한다.
- OIDC, 일부 runner 기능, `actions/cache`, step summary 등은 완전 호환되지 않는다.
