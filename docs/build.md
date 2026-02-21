# 빌드 가이드

Knights의 표준 런타임/검증 환경은 Linux(Docker) 풀스택(`docker/stack`)이다.
Windows는 개발/디버깅(빌드/클라이언트) 용도로 유지한다.

## 전제
- CMake 3.21+ (`CMakePresets.json` 사용)
- C++20 컴파일러
- Python 3 (선택: opcodes/wire codegen 자동 실행)

## 윈도우(Windows) 개발 빌드

### 1) vcpkg 준비(권장, 최초 1회)
```powershell
pwsh scripts/setup_vcpkg.ps1 -Triplet x64-windows
```

### 2) 빌드
```powershell
# 전체 빌드
pwsh scripts/build.ps1 -Config Debug

# 단일 타깃 빌드
pwsh scripts/build.ps1 -Config Debug -Target server_app
```

### 3) 테스트
```powershell
ctest --preset windows-test
```

### 4) 산출물 위치(예시)
- `build-windows/server/Debug/server_app.exe`
- `build-windows/gateway/Debug/gateway_app.exe`
- `build-windows/Debug/wb_worker.exe`

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

- CMake 옵션: `KNIGHTS_ENABLE_SANITIZERS=ON`
- 프리셋: `linux-asan`

로컬 Linux 환경에서:
```bash
cmake --preset linux-asan
cmake --build --preset linux-asan --target server_app --parallel
```

CI에서는 `knights-base` 컨테이너 내부에서 `linux-asan` 빌드를 수행한다. (`.github/workflows/ci.yml` 참고)

## 코드 생성(opcodes)

- 소스(JSON)
  - system(core): `core/protocol/system_opcodes.json`
  - game(server): `server/protocol/game_opcodes.json`
- 생성기
  - 헤더 생성: `tools/gen_opcodes.py`
  - 문서/검증: `tools/gen_opcode_docs.py` → `docs/protocol/opcodes.md`

규칙: 생성된 헤더는 수동으로 편집하지 말고(JSON + generator 수정) 다시 생성한다.
