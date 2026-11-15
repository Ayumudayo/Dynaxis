# 검증

`ctest`는 바이너리를 다시 빌드하지 않는다. 먼저 해당 구성의 빌드를 완료한다.

```powershell
python scripts/build.py --profile full --config Debug --step all
python scripts/build.py --profile full --config Debug --step test
python scripts/build.py --profile core --config Debug --step all
python scripts/build.py --profile core --config Debug --step test
python -m unittest discover -s tests/python -p 'test_*.py'
python tools/check_doxygen_coverage.py
python tools/check_core_api_contracts.py --check-boundary --check-boundary-fixtures --check-stable-governance-fixtures
python tools/gen_opcode_docs.py --check
python tools/check_markdown_links.py
```

전체 검사는 코어의 큐/예외/수명, 인증, 채팅·권한·재접속·history, storage/discovery, 프로토콜 fuzz와 public/installed consumer를 포함한다. `DB_URI`가 없으면 StorageBasic은 skip하므로 실제 Postgres 연결 환경에서 별도로 실행한다. 최소 구성도 독립 TCP smoke와 installed consumer를 검사한다.

Docker 표준 실행은 `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`다. `verify_signed_chat.py --manage-stack`는 signed 모드를 임시 설정하고 복원한다. `verify_core_dependency_recovery.py`는 DB/Redis 중지·복구와 앱 재시작 없는 전달·적재를 검사한다. `verify_continuity_recovery_matrix.py --no-build`는 재시작/failover 후 신원·방 복원을 검사하고 끝에 스택을 내린다.

`verify_pong.py`, `verify_chat.py`, `verify_whisper_cross_instance.py`는 실행 중인 개발 스택의 기본 프로토콜 검사다. `verify_soak_perf_gate.py`는 14-byte frame/HELLO와 오류율·종료 후 큐/세션 수를 검사한다. 짧은 연결 gate를 장시간 채팅 부하 검증으로 해석하지 않는다.

CI는 Windows 빌드/단위/설치 검사, Linux 최소 core, Docker 핵심 종단 검사, Linux sanitizer/fuzz와 ImGui software-rendering smoke를 유지한다. 관리·게임·확장 기능 검사는 제거했다.


## 기초 계약 회귀 검사

- CTest `JobQueueTests`, `ServerStartupFailure`, `ChatServiceTest`, `MessageTimelineTest`: MPMC 진행, 기동 예외, commit 실패, 세션 순서와 snapshot/live 병합.
- `python tests/python/verify_foundation_contracts.py`: 기본 로컬 개발 스택에서 두 backend의 로그인·방 권한, 동시 생성, 확정 ID, deferred commit 실패와 원격 kick을 검사한다. 합성 메시지만 실패시키는 DB trigger를 만들고 finally에서 제거한다. 운영 DB에 실행하지 않는다.
- `python tests/python/verify_gateway_readiness.py`: 개발 registry를 잠시 unready로 바꾸고 gateway의 준비 상태와 실제 로그인 복구를 검사한 뒤 원래 값을 복구한다.
- `python tests/python/verify_core_dependency_recovery.py`: DB/Redis 실제 장애 후 앱 재시작 없는 준비 상태·적재·구독·귓속말 복구를 검사한다.

이 검사들은 `.github/workflows/ci-stack.yml`에서 수행한다. core의 full 설치 소비자는 채팅 Protobuf를 전이 링크하지 않는다. 채팅 wire를 직접 사용하는 외부 소비자는 `find_package(chat_wire CONFIG REQUIRED)`와 `chat_wire::wire_proto`를 명시한다.
