# 테스트 가이드

이 문서는 Dynaxis 저장소의 현재 검증 entrypoint다.
세부 시나리오 카탈로그는 각 도구 문서가 source of truth를 가진다. 특히 loadgen transport proof shape는 `tools/loadgen/README.md`를 따른다.

## 기본 로컬 게이트

```powershell
pwsh scripts/build.ps1 -Config Release
ctest --preset windows-test --output-on-failure
python tools/gen_opcode_docs.py --check
python tools/check_markdown_links.py
```

`core/include/server/core/**`를 건드렸다면 아래를 추가한다.

```powershell
python tools/check_core_api_contracts.py --check-boundary
python tools/check_core_api_contracts.py --check-boundary-fixtures
python tools/check_core_api_contracts.py --check-stable-governance-fixtures
```

## 단위 및 계약 테스트

- 스토리지 기본 검증:
  - `pwsh scripts/build.ps1 -Config Debug -Target storage_basic_tests`
  - `build-windows/tests/Debug/storage_basic_tests.exe`
- 공개 API/계약 검증:
  - `core_public_api_smoke`
  - `core_public_api_headers_compile`
  - `core_public_api_stable_header_scenarios`
- package-first extraction 관련 검증:
  - `ctest -C Debug --test-dir build-windows/tests -R "CoreInstalledPackageConsumer|FactoryPgInstalledPackageConsumer|FactoryRedisInstalledPackageConsumer" --output-on-failure`

## Stack / Integration Verification

- Docker stack:
  - `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`
  - `pwsh scripts/deploy_docker.ps1 -Action down`
- Observability 포함:
  - `pwsh scripts/run_full_stack_observability.ps1`
- write-behind smoke:
  - `scripts/smoke_wb.ps1 -Config Debug -BuildDir build-windows`

## Python Proof Suites

- continuity / restart:
  - `python tests/python/verify_session_continuity.py`
  - `python tests/python/verify_session_continuity_restart.py --scenario gateway-restart`
  - `python tests/python/verify_session_continuity_restart.py --scenario server-restart`
  - `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
  - `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
- FPS transport/runtime:
  - `python tests/python/verify_fps_state_transport.py`
- admin/control-plane:
  - `python tests/python/verify_admin_api.py`
  - `python tests/python/verify_admin_auth.py`
  - `python tests/python/verify_admin_read_only.py`

## Load Generator

- canonical usage and scenario catalog:
  - `tools/loadgen/README.md`
- current supported proof families:
  - TCP chat/ping/login workloads
  - UDP attach, direct ping, FPS input proof
  - RUDP attach/fallback/OFF, direct ping, FPS input proof
- direct UDP/RUDP proofs require direct gateway TCP+UDP ports, not HAProxy frontend routing

## CI Surfaces

- `.github/workflows/ci.yml`
  - Windows build/test + opcode/doc checks
- `.github/workflows/ci-api-governance.yml`
  - core API boundary/governance checks
- `.github/workflows/ci-stack.yml`
  - Docker stack verification
- `.github/workflows/ci-extensibility.yml`
  - extensibility/runtime toggle verification
- `.github/workflows/ci-hardening.yml`
  - sanitizer/fuzz/perf hardening paths
