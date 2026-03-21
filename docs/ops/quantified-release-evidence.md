# 정량 릴리스 증거 기준선

이 문서는 공개 `server_core` 마감선에서 요구하는 Phase 5 정량 증거 기준선을 정의한다.
지금까지 흩어져 있던 proof 스크립트, loadgen 샘플, 로컬 측정 메모를 하나의 명시적 release blocker 목록으로 묶기 위해 존재한다.

## 상태

- Phase 5는 이제 선택적 backlog가 아니라 실제 마감선 blocker다.
- 이 문서는 증거 목록, 권장 실행기, 산출물 위치, 고정 임계치 정책, 최종 수락 체크리스트를 고정한다.
- 여기 적힌 수치는 현재 공개 엔진 마감선에서 받아들인 release blocker 기준선이다.
- 임계치는 반복 측정 후 더 엄격해질 수는 있지만, 기록된 명시적 이유 없이 완화되면 안 된다.

## 산출물 규약

- assertion 스타일 matrix 실행 로그는 `build/phase5-evidence/<run_id>/` 아래에 저장한다.
- loadgen JSON 보고서는 계속 `build/loadgen/` 아래에 둔다.
- 승인된 각 실행은 반드시 아래 네 가지를 같이 남겨야 한다.
  - 정확한 실행 명령
  - 산출물 경로
  - 판정 결과
  - 그 결과를 해석하는 기준 문서
- direct-path 첫 캡처 tranche의 권장 실행기:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id>`
- hardening 재실행 권장 실행기:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening`
- 예약/수동 Linux hardening 권장 실행기:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening --execution-mode hostnet-container`
- 집중 안정화 권장 실행기:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --capture-set rudp-success-only`

## 임계치 정책

- correctness 전용 증거는 하드 패스 규칙을 쓴다.
  - 실행기가 `0`으로 종료해야 한다.
  - 명시된 모든 단계가 성공해야 한다.
- 정량 증거는 현재 기록된 샘플을 계획 기준선으로 삼아 시작한다.
  - 지연시간 guard: `current_sample_p95_ms * 1.25`
  - 처리량 guard: `current_sample_throughput_rps * 0.80`
  - 오류 guard: `errors=0`
  - 적용되는 경우 transport attach guard: `attach_failures=0`
- 재실행 결과가 충분히 안정적이면 아래 섹션의 명시적 고정 release threshold로 승격한다.

## 릴리스 증거 목록

| 증거 ID | 반드시 증명해야 하는 것 | 권장 실행기 | 산출물 경로 | 해석 기준 문서 |
| --- | --- | --- | --- | --- |
| `transport-impairment-matrix` | direct UDP/RUDP attach, OFF, rollout fallback, protocol fallback, restart, 결정론적 packet-quality impairment가 모두 올바르게 동작함 | `python tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance --no-build *> build/phase5-evidence/<run_id>/fps/phase2-acceptance.log` | `build/phase5-evidence/<run_id>/fps/phase2-acceptance.log` | `docs/ops/realtime-runtime-contract.md` |
| `mixed-transport-soak` | 긴 mixed TCP + direct UDP/RUDP 트래픽이 attach, fallback, OFF 정책 모드에서 깨끗하게 유지됨 | `tools/loadgen/README.md`의 `mixed_session_soak_long`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long` 및 fallback/OFF 변형 명령 | `build/loadgen/mixed_session_soak_long.json`, `build/loadgen/mixed_direct_udp_soak_long.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.fallback.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.off.host.json` | 이 문서 |
| `fps-direct-path-budget` | gameplay-frequency direct path가 direct UDP와 RUDP에서 허용 가능한 지연시간/처리량/오류 행동을 유지함 | `tools/loadgen/README.md`의 `mixed_direct_udp_ping_soak`, `mixed_direct_rudp_ping_soak`, `mixed_direct_udp_fps_soak`, `mixed_direct_rudp_fps_soak` | `build/loadgen/mixed_direct_udp_ping_soak.host.json`, `build/loadgen/mixed_direct_rudp_ping_soak.host.json`, `build/loadgen/mixed_direct_udp_fps_soak.host.json`, `build/loadgen/mixed_direct_rudp_fps_soak.host.json` | `docs/ops/realtime-runtime-contract.md` |
| `mmorpg-handoff-rehearsal` | desired/observed topology, drain closure, owner transfer, migration handoff, live runtime assignment가 한 지원 행렬 안에서 재현 가능함 | `python tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance --no-build *> build/phase5-evidence/<run_id>/mmorpg/phase3-acceptance.log` | `build/phase5-evidence/<run_id>/mmorpg/phase3-acceptance.log` | `docs/ops/mmorpg-world-residency-contract.md` |
| `continuity-restart-recovery` | gateway/server 재시작과 continuity fallback 동작이 하나의 이름 붙은 recovery runner로 재현 가능함 | `python tests/python/verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline --no-build *> build/phase5-evidence/<run_id>/continuity/phase5-recovery-baseline.log` | `build/phase5-evidence/<run_id>/continuity/phase5-recovery-baseline.log` | `docs/ops/session-continuity-contract.md` |

## 잠정 기준선 구간

### 전송 손상 행렬

- gate 유형: correctness
- 요구 결과:
  - `phase2-acceptance` 실행기가 `0`으로 종료해야 한다.
  - 명시된 `rudp-attach`, `udp-only-off`, `rollout-fallback`, `protocol-fallback`, `udp-quality-impairment`, `rudp-restart` 단계가 모두 통과해야 한다.
- 첫 캡처 실행:
  - `build/phase5-evidence/20260318-010307Z/fps/phase2-acceptance.log`
- 고정 release threshold:
  - 수치 임계치가 아니라 pass/fail만 본다. 첫 캡처 실행만으로도 correctness gate로 유지하기에 충분했다.
- 현재 알려진 한계:
  - 더 넓은 OS-level `netem` rehearsal은 아직 없다. 이는 첫 기준선 구간이 아니라 후속 증거 확장 항목이다.

### 긴 mixed soak

- 기준 샘플은 이미 `tasks/validation/quantitative/todo.md`에 기록돼 있다.
- hardening 재실행:
  - `build/loadgen/mixed_session_soak_long.20260318-021023Z.json`
  - `build/loadgen/mixed_direct_udp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.fallback.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.off.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260317-173024Z.host.json`
- 고정 release threshold:
  - `mixed_session_soak_long`: `p95_ms <= 16.50`, `throughput_rps >= 8.60`, `errors=0`
  - `mixed_direct_udp_soak_long`: `p95_ms <= 15.00`, `throughput_rps >= 8.00`, `errors=0`, `attach_failures=0`, `udp_bind_successes>0`
  - `mixed_direct_rudp_soak_long` 성공 경로: `p95_ms <= 17.50`, `throughput_rps >= 8.00`, `errors=0`, `attach_failures=0`, `rudp_attach_successes>0`, `rudp_attach_fallbacks=0`
  - `mixed_direct_rudp_soak_long` fallback/OFF 정책: `p95_ms <= 15.70`, `throughput_rps >= 8.00`, `errors=0`, `attach_failures=0`, `rudp_attach_successes=0`, `rudp_attach_fallbacks>0`

### FPS direct-path 지연시간 / 처리량 / 오류 예산

- 현재 direct ping workload의 기록 샘플:
  - `mixed_direct_udp_ping_soak`: 기준선 `p95_ms=11.89`, `throughput_rps=8.19`
  - `mixed_direct_rudp_ping_soak` 성공 경로: 기준선 `p95_ms=78.26`, `throughput_rps=7.93`
  - `mixed_direct_rudp_ping_soak` fallback/OFF 경로: 기준선 `p95_ms=11.95`, `throughput_rps=7.43`
- 잠정 계획 guard:
  - UDP direct ping soak: `p95_ms <= 14.87`, `throughput_rps >= 6.55`, `errors=0`, `attach_failures=0`
  - RUDP direct ping soak 성공 경로: `p95_ms <= 97.83`, `throughput_rps >= 6.34`, `errors=0`, `attach_failures=0`
  - RUDP direct ping soak fallback/OFF 경로: `p95_ms <= 14.94`, `throughput_rps >= 5.94`, `errors=0`, `attach_failures=0`
- 첫 FPS soak 보고서:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-010307Z.host.json`
    - 기준선 `p95_ms=31.62`, `throughput_rps=8.04`, `errors=0`, `attach_failures=0`
    - 잠정 guard `p95_ms <= 39.52`, `throughput_rps >= 6.43`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-010307Z.host.json`
    - 기준선 `p95_ms=31.60`, `throughput_rps=8.02`, `errors=0`, `attach_failures=0`
    - 잠정 guard `p95_ms <= 39.50`, `throughput_rps >= 6.42`
- hardening 재실행:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-021023Z.host.json`
- 고정 release threshold:
  - UDP direct FPS soak: `p95_ms <= 36.50`, `throughput_rps >= 6.80`, `errors=0`, `attach_failures=0`, `udp_bind_successes>0`
  - RUDP direct FPS soak 성공 경로: `p95_ms <= 36.50`, `throughput_rps >= 6.80`, `errors=0`, `attach_failures=0`, `udp_bind_successes>0`, `rudp_attach_successes>0`, `rudp_attach_fallbacks=0`

### MMORPG handoff rehearsal

- gate 유형: correctness
- 요구 결과:
  - `phase3-acceptance` 실행기가 `0`으로 종료해야 한다.
  - topology-aware 단계가 모두 통과해야 한다.
- 첫 캡처 실행:
  - `build/phase5-evidence/20260318-010307Z/mmorpg/phase3-acceptance.log`
- 고정 release threshold:
  - 수치가 아니라 pass/fail만 본다. 첫 실행만으로 correctness gate로 유지하기에 충분했다.
- 해석 기준 문서:
  - `docs/ops/mmorpg-world-residency-contract.md`

### continuity 보존 재시작 / 복구

- gate 유형: correctness
- 요구 결과:
  - `phase5-recovery-baseline` 실행기가 `0`으로 종료해야 한다.
  - `gateway-restart`, `server-restart`, `locator-fallback`, `world-residency-fallback`, `world-owner-fallback`이 모두 한 시퀀스 안에서 통과해야 한다.
- 첫 캡처 실행:
  - `build/phase5-evidence/20260318-010307Z/continuity/phase5-recovery-baseline.log`
- 고정 release threshold:
  - 수치가 아니라 pass/fail만 본다. 첫 실행만으로 correctness gate로 유지하기에 충분했다.
- 해석 기준 문서:
  - `docs/ops/session-continuity-contract.md`

## 첫 캡처 결과 (`20260318-010307Z`)

- assertion 스타일 correctness 로그:
  - `build/phase5-evidence/20260318-010307Z/fps/phase2-acceptance.log`
  - `build/phase5-evidence/20260318-010307Z/mmorpg/phase3-acceptance.log`
  - `build/phase5-evidence/20260318-010307Z/continuity/phase5-recovery-baseline.log`
- direct-path loadgen 보고서:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-010307Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-010307Z.host.json`
- 요약:
  - 세 correctness runner가 모두 통과했다.
  - 두 FPS direct-path soak 모두 `errors=0`, `attach_failures=0`으로 종료했다.
  - FPS direct-path 정량 예산은 아직 잠정 상태이며 다음 hardening tranche로 넘어간다.

## hardening 캡처 결과 (`20260318-021023Z`)

- 긴 mixed soak 보고서:
  - `build/loadgen/mixed_session_soak_long.20260318-021023Z.json`
  - `build/loadgen/mixed_direct_udp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.fallback.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.off.host.json`
- FPS direct-path 재실행 보고서:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-021023Z.host.json`
- 결과:
  - `mixed_session_soak_long`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long` fallback/OFF, 그리고 두 FPS direct-path 성공 경로는 이제 고정 threshold가 되었다.
  - `mixed_direct_rudp_soak_long` 성공 경로는 threshold를 고정하기 전에 추가 집중 재실행이 한 번 더 필요했다.

## 집중 안정화 캡처 결과 (`20260317-173024Z`)

- 집중 보고서:
  - `build/phase5-evidence/20260317-173024Z/manifest.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260317-173024Z.host.json`
- 결과:
  - `mixed_direct_rudp_soak_long` 성공 경로가 `p95_ms=13.40`, `throughput_rps=8.92`, `errors=0`, `rudp_attach_successes=8`, `rudp_attach_fallbacks=0`으로 재실행되었다.
  - 이전에 넓어졌던 `p95_ms=16.96` 샘플은 새로운 정상 기준이 아니라 suite 수준 분산으로 본다.
  - 따라서 성공 경로는 `p95_ms <= 17.50`의 고정 release threshold로 승격되었다.

## 이식 가능한 컨테이너 진단 결과 (`20260317-174844Z`)

- 이식 가능한 전체 예산 보고서:
  - `build/phase5-evidence/20260317-174844Z/manifest.json`
- 결과:
  - 동일 네트워크 Linux 컨테이너 실행은 긴 mixed soak threshold를 계속 만족한다.
    - `mixed_session_soak_long`: `p95_ms=13.73`, `throughput_rps=9.58`
    - `mixed_direct_udp_soak_long`: `p95_ms=13.47`, `throughput_rps=8.95`
    - `mixed_direct_rudp_soak_long` 성공 경로: `p95_ms=15.46`, `throughput_rps=8.93`
    - `mixed_direct_rudp_soak_long` fallback/OFF: `p95_ms=13.00` / `13.60`
  - 같은 컨테이너 경로는 받아들인 FPS direct-path host 기준선은 재현하지 못했다.
    - `mixed_direct_udp_fps_soak`: `p95_ms=43.21`, `throughput_rps=8.05`
    - `mixed_direct_rudp_fps_soak`: `p95_ms=43.24`, `throughput_rps=8.09`
  - 그래서 현재 release threshold는 계속 host 스타일 캡처를 기준으로 유지하고, portable container 모드는 release 자동화가 아니라 진단 경로로 남긴다.

## hostnet 자동화 기준선 결과 (`20260318-060310Z`)

- hostnet 산출물:
  - `build/phase5-evidence/20260318-060310Z/manifest.json`
- 결과:
  - `hostnet-container`는 받아들인 FPS direct-path 기준선을 충분히 가깝게 재현해 예약/수동 Linux hardening 산출물 수집 경로로 승격되었다.
    - `mixed_direct_udp_fps_soak`: `p95_ms=31.97`, `throughput_rps=8.08`
    - `mixed_direct_rudp_fps_soak`: `p95_ms=32.32`, `throughput_rps=8.03`
  - 긴 mixed soak도 허용된 release band 안에 남았다.
    - `mixed_session_soak_long`: `p95_ms=13.37`, `throughput_rps=9.61`
    - `mixed_direct_udp_soak_long`: `p95_ms=12.78`, `throughput_rps=8.97`
    - `mixed_direct_rudp_soak_long` 성공 경로: `p95_ms=12.22`, `throughput_rps=8.91`
    - `mixed_direct_rudp_soak_long` fallback 경로: `p95_ms=12.60`, `throughput_rps=8.91`
  - OFF-path 샘플 하나가 `p95_ms=16.47`로 넓어졌지만, 즉시 집중 재실행한 hostnet retest는 `p95_ms=12.75`로 돌아왔다.
  - 결정:
    - `hostnet-container`를 `ci-hardening`의 예약/수동 hardening 산출물 경로로 승격한다.
    - release 해석은 여전히 위의 기준선 band와 업로드된 산출물에 대한 사람 검토를 함께 사용한다.

## hostnet OFF 이력 구간 (`20260318-113911Z-off1..off3`)

- 집중 OFF-path 보고서:
  - `build/phase5-evidence/20260318-113911Z-off1/manifest.json`
  - `build/phase5-evidence/20260318-113911Z-off2/manifest.json`
  - `build/phase5-evidence/20260318-113911Z-off3/manifest.json`
- 결과:
  - 반복 실행한 hostnet OFF-path는 계속 허용된 fallback/OFF band 안에 머물렀다.
    - `off1`: `p95_ms=11.9996`, `throughput_rps=8.9320`
    - `off2`: `p95_ms=13.3200`, `throughput_rps=8.8952`
    - `off3`: `p95_ms=12.1303`, `throughput_rps=8.9103`
  - 앞서 보였던 `p95_ms=16.4659` 샘플은 Linux hardening 전용 두 번째 수치 band를 만들 근거가 아니라 고립된 실행 분산으로 해석한다.
  - 결정:
    - 지금은 Linux 전용 hardening threshold band를 따로 고정하지 않는다.
    - 기존 fallback/OFF release band와 업로드 산출물 검토 방식을 계속 사용한다.

## 자동화 결정

- 경로 기반 stack 자동화:
  - `phase2-acceptance`
  - `phase3-acceptance`
  - `phase5-recovery-baseline`
- 예약/수동 hardening 자동화:
  - `.github/workflows/ci-hardening.yml`의 `phase5-budget-evidence` 산출물 수집
  - 실행기: `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening --execution-mode hostnet-container`
- 현재는 release 전용 / 수동 해석으로 남기는 것:
  - 긴 mixed soak 예산 세트
  - FPS direct-path 성능 예산 세트
- 이유:
  - correctness runner는 이미 충분히 싸고, `ci-stack`에서 이진 pass/fail gate로 돌릴 수 있다.
  - `--execution-mode container`는 Linux 동일 네트워크 진단에는 여전히 유용하지만, FPS direct-path `p95_ms`를 받아들인 host 기준선보다 넓힌다.
  - `--execution-mode hostnet-container`는 받아들인 기준선에 충분히 가깝고, 현재 OFF-path 이력도 Linux 전용 두 번째 threshold band를 정당화하지 않는다.
  - `--capture-set rudp-success-only`는 집중 진단용 재실행 도구로 남기고, 새 CI gate로 만들지는 않는다.

## 최종 수락 체크리스트

- 공개 package/API governance가 계속 green 상태여야 한다.
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`
  - `ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure`
- correctness matrix가 하나의 이름 붙은 실행 안에서 계속 green이어야 한다.
  - `python tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance --no-build *> build/phase5-evidence/<run_id>/fps/phase2-acceptance.log`
  - `python tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance --no-build *> build/phase5-evidence/<run_id>/mmorpg/phase3-acceptance.log`
  - `python tests/python/verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline --no-build *> build/phase5-evidence/<run_id>/continuity/phase5-recovery-baseline.log`
- 정량 예산 캡처가 위의 고정 band 안에 머물러야 한다.
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening`
- RUDP mixed success path만 다시 확인하면 될 때는 집중 재실행 도구를 쓴다.
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --capture-set rudp-success-only`

## 종료 후 후속 과제

- 결정론적 손상 proof에서 더 나아가, 더 넓은 OS-level `netem` 또는 lossy-network rehearsal로 확장
  - 권장 수동 실행기: `python tests/python/verify_fps_netem_rehearsal.py --scenario fps-pair`
  - 현재 위치: `ci-hardening`이 아니라 수동 ops 전용 경로
