# 로컬 CI 가이드 (act)

이 문서는 GitHub Actions 워크플로우를 `push` 전에 로컬에서 검증하기 위한 표준 절차를 정의합니다.

## 목적
- 워크플로우 문법/스크립트 오류를 원격 CI에서 늦게 발견하는 문제를 줄인다.
- 큰 변경은 로컬 CI를 먼저 통과시켜 불필요한 푸시-재시도 루프를 줄인다.

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
act pull_request -W .github/workflows/ci.yml -j core-api-consumer-linux --dryrun
act pull_request -W .github/workflows/ci.yml -j windows-conan-deps --dryrun
```

## Push 전 게이트 (큰 변경)
다음 조건 중 하나라도 만족하면 "큰 변경"으로 간주합니다.
- CMake/Conan/워크플로우/빌드 스크립트 변경
- 3개 이상 모듈(`core/`, `server/`, `gateway/`, `tools/`) 동시 변경
- 런타임/의존성/코드 생성 경로 변경

큰 변경은 아래 로컬 CI를 통과한 뒤 push 합니다.

```bash
act pull_request -W .github/workflows/ci.yml -j core-api-consumer-linux
act pull_request -W .github/workflows/ci.yml -j windows-fast-tests
```

`core/include/server/core/**`를 건드렸다면 아래도 추가로 실행합니다.

```bash
act pull_request -W .github/workflows/ci.yml -j core-api-consumer-windows
```

## 운영 규칙
- 로컬 CI 실패 상태에서는 push하지 않는다.
- 실패 시 로그를 기준으로 수정 후 동일 잡을 재실행한다.
- 원격 CI는 로컬 CI를 통과한 커밋을 검증하는 최종 단계로 사용한다.

## 제한 사항
- `act`는 GitHub hosted runner와 100% 동일하지 않다.
- Windows 잡은 `-self-hosted`로 호스트에서 실행되므로 로컬 환경 영향에 주의한다.
- OIDC, 일부 runner 기능, step summary 등은 완전 호환되지 않는다.
