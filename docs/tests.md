# 검증

`ctest`는 바이너리를 다시 빌드하지 않는다. 먼저 해당 구성의 빌드를 완료한다.

```powershell
cmake --build build-windows --config Debug --parallel 4
ctest --test-dir build-windows -C Debug --output-on-failure --no-tests=error
cmake --build --preset windows-core-engine-debug
ctest --preset windows-core-engine-test --no-tests=error
python -m unittest discover -s tests/python -p 'test_*.py'
python tools/check_doxygen_coverage.py
python tools/check_core_api_contracts.py --check-boundary --check-boundary-fixtures --check-stable-governance-fixtures
python tools/gen_opcode_docs.py --check
python tools/check_markdown_links.py
```

전체 검사는 코어의 큐/예외/수명, 인증, 채팅·권한·재접속·history, storage/discovery, 프로토콜 fuzz와 public/installed consumer를 포함한다. `DB_URI`가 없으면 StorageBasic은 skip하므로 실제 Postgres 연결 환경에서 별도로 실행한다. 최소 구성도 독립 TCP smoke와 installed consumer를 검사한다.

Docker 표준 실행은 `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`다. `verify_signed_chat.py --manage-stack`는 signed 모드를 임시 설정하고 복원한다. `verify_core_dependency_recovery.py`는 DB/Redis 중지·복구와 앱 재시작 없는 전달·적재를 검사한다. `verify_continuity_recovery_matrix.py --no-build`는 재시작/failover 후 신원·방 복원을 검사하고 끝에 스택을 내린다.

`verify_pong.py`, `verify_chat.py`, `verify_whisper_cross_instance.py`는 실행 중인 개발 스택의 기본 프로토콜 검사다. `verify_soak_perf_gate.py`는 14-byte frame/HELLO와 오류율·종료 후 큐/세션 수를 검사한다. 짧은 연결 gate를 장시간 채팅 부하 검증으로 해석하지 않는다.

CI는 Windows 빌드/단위/설치 검사, Linux 최소 core, Docker 핵심 종단 검사, Linux sanitizer/fuzz를 유지한다. 관리·게임·확장 기능 검사는 제거했다.
