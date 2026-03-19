# Quantified Release Evidence Baseline

This document defines the Phase 5 release-evidence baseline for the public `server_core` finish line.
It exists to convert the current mixture of proof scripts, loadgen samples, and local measurement notes into one explicit release-blocker inventory.

## Status

- Phase 5 is now a finish-line blocker, not an optional backlog.
- This document fixes the evidence inventory, preferred runner, artifact location, fixed threshold policy, and final acceptance checklist.
- The numbers here are the current accepted release-blocker baseline for the public-engine finish line.
- Thresholds may tighten after repeated reruns, but they must not become looser without an explicit recorded reason.

## Artifact Convention

- assertion-style matrix runs store logs under `build/phase5-evidence/<run_id>/`
- loadgen JSON reports continue to live under `build/loadgen/`
- each accepted run must record:
  - the exact command
  - the artifact path
  - the verdict
  - the committed document that owns interpretation of the result
- preferred direct-path report runner for the first capture tranche:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id>`
- preferred hardening rerun runner:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening`
- preferred scheduled/manual Linux hardening runner:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening --execution-mode hostnet-container`
- preferred focused stabilization runner:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --capture-set rudp-success-only`

## Threshold Policy

- correctness-only evidence uses a hard pass rule:
  - runner exits `0`
  - all named stages succeed
- quantitative evidence starts from the current recorded sample as a planning baseline:
  - latency guard: `current_sample_p95_ms * 1.25`
  - throughput guard: `current_sample_throughput_rps * 0.80`
  - error guard: `errors=0`
  - transport attach guard where applicable: `attach_failures=0`
- once reruns are stable enough, the item is promoted into an explicit fixed release threshold in the sections below.

## Release-Evidence Inventory

| Evidence ID | What It Must Prove | Preferred Runner | Artifact Path | Committed Owner |
| --- | --- | --- | --- | --- |
| `transport-impairment-matrix` | direct UDP/RUDP attach, OFF, rollout fallback, protocol fallback, restart, deterministic packet-quality impairment all stay correct | `python tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance --no-build *> build/phase5-evidence/<run_id>/fps/phase2-acceptance.log` | `build/phase5-evidence/<run_id>/fps/phase2-acceptance.log` | `docs/ops/realtime-runtime-contract.md` |
| `mixed-transport-soak` | long mixed TCP + direct UDP/RUDP traffic remains clean under attach, fallback, and OFF policy modes | loadgen commands from `tools/loadgen/README.md` for `mixed_session_soak_long`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long`, plus fallback/OFF env variants | `build/loadgen/mixed_session_soak_long.json`, `build/loadgen/mixed_direct_udp_soak_long.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.fallback.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.off.host.json` | this document |
| `fps-direct-path-budget` | gameplay-frequency direct path keeps acceptable latency/throughput/error behavior on direct UDP and RUDP | loadgen commands from `tools/loadgen/README.md` for `mixed_direct_udp_ping_soak`, `mixed_direct_rudp_ping_soak`, `mixed_direct_udp_fps_soak`, `mixed_direct_rudp_fps_soak` | `build/loadgen/mixed_direct_udp_ping_soak.host.json`, `build/loadgen/mixed_direct_rudp_ping_soak.host.json`, `build/loadgen/mixed_direct_udp_fps_soak.host.json`, `build/loadgen/mixed_direct_rudp_fps_soak.host.json` | `docs/ops/realtime-runtime-contract.md` |
| `mmorpg-handoff-rehearsal` | desired/observed topology, drain closure, owner transfer, migration handoff, and live runtime assignment remain reproducible in one supported matrix | `python tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance --no-build *> build/phase5-evidence/<run_id>/mmorpg/phase3-acceptance.log` | `build/phase5-evidence/<run_id>/mmorpg/phase3-acceptance.log` | `docs/ops/mmorpg-world-residency-contract.md` |
| `continuity-restart-recovery` | gateway/server restart and continuity fallback behavior remain reproducible through one named recovery runner | `python tests/python/verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline --no-build *> build/phase5-evidence/<run_id>/continuity/phase5-recovery-baseline.log` | `build/phase5-evidence/<run_id>/continuity/phase5-recovery-baseline.log` | `docs/ops/session-continuity-contract.md` |

## Provisional Baseline Slice

### Transport Impairment Matrix

- gate type: correctness
- required result:
  - `phase2-acceptance` runner exits `0`
  - named stages `rudp-attach`, `udp-only-off`, `rollout-fallback`, `protocol-fallback`, `udp-quality-impairment`, `rudp-restart` all pass
- first captured run:
  - `build/phase5-evidence/20260318-010307Z/fps/phase2-acceptance.log`
- fixed release threshold:
  - pass/fail only; the first captured run was sufficient to keep this as a hard correctness gate
- current known limitation:
  - fuller OS-level `netem` rehearsal is still missing and remains a follow-up evidence expansion, not part of this first baseline slice

### Long Mixed Soak

- baseline samples already recorded in `tasks/validation/quantitative/todo.md`
- hardening rerun:
  - `build/loadgen/mixed_session_soak_long.20260318-021023Z.json`
  - `build/loadgen/mixed_direct_udp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.fallback.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.off.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260317-173024Z.host.json`
- fixed release thresholds:
  - `mixed_session_soak_long`: `p95_ms <= 16.50`, `throughput_rps >= 8.60`, `errors=0`
  - `mixed_direct_udp_soak_long`: `p95_ms <= 15.00`, `throughput_rps >= 8.00`, `errors=0`, `attach_failures=0`, `udp_bind_successes>0`
  - `mixed_direct_rudp_soak_long` success path: `p95_ms <= 17.50`, `throughput_rps >= 8.00`, `errors=0`, `attach_failures=0`, `rudp_attach_successes>0`, `rudp_attach_fallbacks=0`
  - `mixed_direct_rudp_soak_long` fallback/OFF policy: `p95_ms <= 15.70`, `throughput_rps >= 8.00`, `errors=0`, `attach_failures=0`, `rudp_attach_successes=0`, `rudp_attach_fallbacks>0`

### FPS Direct-Path Latency / Throughput / Error Budget

- current recorded samples exist for direct ping workload:
  - `mixed_direct_udp_ping_soak`: baseline `p95_ms=11.89`, `throughput_rps=8.19`
  - `mixed_direct_rudp_ping_soak` success path: baseline `p95_ms=78.26`, `throughput_rps=7.93`
  - `mixed_direct_rudp_ping_soak` fallback/OFF path: baseline `p95_ms=11.95`, `throughput_rps=7.43`
- provisional planning guards:
  - UDP direct ping soak: `p95_ms <= 14.87`, `throughput_rps >= 6.55`, `errors=0`, `attach_failures=0`
  - RUDP direct ping soak success path: `p95_ms <= 97.83`, `throughput_rps >= 6.34`, `errors=0`, `attach_failures=0`
  - RUDP direct ping soak fallback/OFF path: `p95_ms <= 14.94`, `throughput_rps >= 5.94`, `errors=0`, `attach_failures=0`
- first captured FPS soak reports:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-010307Z.host.json`
    - baseline `p95_ms=31.62`, `throughput_rps=8.04`, `errors=0`, `attach_failures=0`
    - provisional guard `p95_ms <= 39.52`, `throughput_rps >= 6.43`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-010307Z.host.json`
    - baseline `p95_ms=31.60`, `throughput_rps=8.02`, `errors=0`, `attach_failures=0`
    - provisional guard `p95_ms <= 39.50`, `throughput_rps >= 6.42`
- hardening rerun:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-021023Z.host.json`
- fixed release thresholds:
  - UDP direct FPS soak: `p95_ms <= 36.50`, `throughput_rps >= 6.80`, `errors=0`, `attach_failures=0`, `udp_bind_successes>0`
  - RUDP direct FPS soak success path: `p95_ms <= 36.50`, `throughput_rps >= 6.80`, `errors=0`, `attach_failures=0`, `udp_bind_successes>0`, `rudp_attach_successes>0`, `rudp_attach_fallbacks=0`

### MMORPG Handoff Rehearsal

- gate type: correctness
- required result:
  - `phase3-acceptance` runner exits `0`
  - both topology-aware stages pass
- first captured run:
  - `build/phase5-evidence/20260318-010307Z/mmorpg/phase3-acceptance.log`
- fixed release threshold:
  - pass/fail only; the first captured run was sufficient to keep this as a hard correctness gate
- interpretation owner:
  - `docs/ops/mmorpg-world-residency-contract.md`

### Restart / Recovery With Continuity Preservation

- gate type: correctness
- required result:
  - `phase5-recovery-baseline` runner exits `0`
  - `gateway-restart`, `server-restart`, `locator-fallback`, `world-residency-fallback`, `world-owner-fallback` all pass in one repeatable sequence
- first captured run:
  - `build/phase5-evidence/20260318-010307Z/continuity/phase5-recovery-baseline.log`
- fixed release threshold:
  - pass/fail only; the first captured run was sufficient to keep this as a hard correctness gate
- interpretation owner:
  - `docs/ops/session-continuity-contract.md`

## First Capture Result (`20260318-010307Z`)

- assertion-style correctness logs:
  - `build/phase5-evidence/20260318-010307Z/fps/phase2-acceptance.log`
  - `build/phase5-evidence/20260318-010307Z/mmorpg/phase3-acceptance.log`
  - `build/phase5-evidence/20260318-010307Z/continuity/phase5-recovery-baseline.log`
- direct-path loadgen reports:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-010307Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-010307Z.host.json`
- capture summary:
  - all three correctness runners passed
  - both FPS direct-path soak runs completed with `errors=0` and `attach_failures=0`
  - quantitative budgets for FPS direct-path remain provisional and move to the next hardening tranche

## Hardening Capture Result (`20260318-021023Z`)

- long mixed soak reports:
  - `build/loadgen/mixed_session_soak_long.20260318-021023Z.json`
  - `build/loadgen/mixed_direct_udp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.fallback.host.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260318-021023Z.off.host.json`
- FPS direct-path rerun reports:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-021023Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-021023Z.host.json`
- outcome:
  - `mixed_session_soak_long`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long` fallback/OFF, and both FPS direct-path soak success paths are now fixed thresholds
  - `mixed_direct_rudp_soak_long` success path required one narrower follow-up rerun before its threshold could be fixed

## Focused Stabilization Capture Result (`20260317-173024Z`)

- focused report:
  - `build/phase5-evidence/20260317-173024Z/manifest.json`
  - `build/loadgen/mixed_direct_rudp_soak_long.20260317-173024Z.host.json`
- outcome:
  - `mixed_direct_rudp_soak_long` success path reran at `p95_ms=13.40`, `throughput_rps=8.92`, `errors=0`, `rudp_attach_successes=8`, `rudp_attach_fallbacks=0`
  - the previously widened `p95_ms=16.96` sample is now treated as bounded suite-level variance rather than the new steady-state baseline
  - the success path is therefore promoted to a fixed release threshold at `p95_ms <= 17.50`

## Portable Container Diagnostic Result (`20260317-174844Z`)

- portable full-budget report:
  - `build/phase5-evidence/20260317-174844Z/manifest.json`
- outcome:
  - same-network Linux container execution keeps the long mixed soak thresholds green:
    - `mixed_session_soak_long`: `p95_ms=13.73`, `throughput_rps=9.58`
    - `mixed_direct_udp_soak_long`: `p95_ms=13.47`, `throughput_rps=8.95`
    - `mixed_direct_rudp_soak_long` success path: `p95_ms=15.46`, `throughput_rps=8.93`
    - `mixed_direct_rudp_soak_long` fallback/OFF: `p95_ms=13.00` / `13.60`
  - the same containerized path does not preserve the accepted FPS direct-path host baseline:
    - `mixed_direct_udp_fps_soak`: `p95_ms=43.21`, `throughput_rps=8.05`
    - `mixed_direct_rudp_fps_soak`: `p95_ms=43.24`, `throughput_rps=8.09`
  - current accepted release thresholds therefore remain tied to the existing host-style capture baseline, and the portable container mode is retained as a diagnostic path rather than the new release automation default

## Hostnet Automation Baseline Result (`20260318-060310Z`)

- hostnet artifact set:
  - `build/phase5-evidence/20260318-060310Z/manifest.json`
- outcome:
  - `hostnet-container` reproduced the accepted FPS direct-path baseline closely enough for scheduled/manual Linux hardening artifact capture:
    - `mixed_direct_udp_fps_soak`: `p95_ms=31.97`, `throughput_rps=8.08`
    - `mixed_direct_rudp_fps_soak`: `p95_ms=32.32`, `throughput_rps=8.03`
  - long mixed soak stayed within the accepted release bands:
    - `mixed_session_soak_long`: `p95_ms=13.37`, `throughput_rps=9.61`
    - `mixed_direct_udp_soak_long`: `p95_ms=12.78`, `throughput_rps=8.97`
    - `mixed_direct_rudp_soak_long` success path: `p95_ms=12.22`, `throughput_rps=8.91`
    - `mixed_direct_rudp_soak_long` fallback path: `p95_ms=12.60`, `throughput_rps=8.91`
  - one OFF-path sample widened to `p95_ms=16.47`, but an immediate focused hostnet retest returned `p95_ms=12.75`
  - decision:
    - `hostnet-container` is promoted as the scheduled/manual hardening artifact path in `ci-hardening`
    - release interpretation still uses the accepted baseline bands above plus human review of the uploaded artifacts

## Hostnet OFF History Slice (`20260318-113911Z-off1..off3`)

- focused OFF-path reports:
  - `build/phase5-evidence/20260318-113911Z-off1/manifest.json`
  - `build/phase5-evidence/20260318-113911Z-off2/manifest.json`
  - `build/phase5-evidence/20260318-113911Z-off3/manifest.json`
- outcome:
  - repeated focused hostnet OFF-path reruns stayed inside the accepted fallback/OFF band:
    - `off1`: `p95_ms=11.9996`, `throughput_rps=8.9320`
    - `off2`: `p95_ms=13.3200`, `throughput_rps=8.8952`
    - `off3`: `p95_ms=12.1303`, `throughput_rps=8.9103`
  - the earlier `p95_ms=16.4659` sample is therefore treated as isolated run-to-run variance rather than evidence that Linux hardening needs a separate second numeric band
  - decision:
    - do not fix a Linux-only hardening threshold band at this time
    - continue using the accepted fallback/OFF release band and uploaded-artifact review for scheduled/manual hardening automation

## Automation Decision

- path-gated stack automation:
  - `phase2-acceptance`
  - `phase3-acceptance`
  - `phase5-recovery-baseline`
- scheduled/manual hardening automation:
  - `phase5-budget-evidence` artifact capture in `.github/workflows/ci-hardening.yml`
  - runner: `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening --execution-mode hostnet-container`
- release-only / manual interpretation for now:
  - long mixed soak budget set
  - FPS direct-path performance budget set
- rationale:
  - correctness runners are already cheap enough and binary-pass in `ci-stack`
  - `--execution-mode container` remains useful for Linux same-network diagnostics, but it widens FPS direct-path `p95_ms` beyond the accepted host baseline
  - `--execution-mode hostnet-container` is close enough to the accepted baseline to automate artifact capture, and current focused OFF-path history does not justify a second Linux-only threshold band
  - `--capture-set rudp-success-only` is retained as a focused diagnostic rerun, not a new CI gate

## Final Acceptance Checklist

- public package/API governance stays green:
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/check_core_api_contracts.py --check-boundary-fixtures`
  - `python tools/check_core_api_contracts.py --check-stable-governance-fixtures`
  - `ctest --test-dir build-windows -C Debug -R "CoreInstalledPackageConsumer|CoreApiBoundaryFixtures|CoreApiStableGovernanceFixtures" --output-on-failure`
- correctness matrices stay green under one named run:
  - `python tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance --no-build *> build/phase5-evidence/<run_id>/fps/phase2-acceptance.log`
  - `python tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance --no-build *> build/phase5-evidence/<run_id>/mmorpg/phase3-acceptance.log`
  - `python tests/python/verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline --no-build *> build/phase5-evidence/<run_id>/continuity/phase5-recovery-baseline.log`
- quantitative budget capture stays within the fixed bands above:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --include-budget-hardening`
- focused rerun tool when only the RUDP mixed success path needs reconfirmation:
  - `python tests/python/capture_phase5_evidence.py --run-id <run_id> --capture-set rudp-success-only`

## Post-Closure Follow-Up

- expand from deterministic impairment proof to fuller OS-level `netem` or lossy-network rehearsal
  - preferred manual runner: `python tests/python/verify_fps_netem_rehearsal.py --scenario fps-pair`
  - current placement: manual ops-only path, not `ci-hardening`
  - validated rehearsal:
    - `build/phase5-evidence/20260318-121332Z/netem/manifest.json`
    - UDP direct FPS under netem: `p95_ms=75.25`, `throughput_rps=7.94`, `errors=0`
    - RUDP direct FPS under netem: `p95_ms=74.71`, `throughput_rps=7.99`, `errors=0`
    - gateway metric deltas: `loss_delta=4`, `jitter_after_ms=37`
- periodically review accumulated hostnet artifacts for new drift before changing any accepted threshold bands
- accumulate Windows compile-cache evidence before making an adopt/no-go call on `sccache`
  - preferred workflow: `.github/workflows/windows-sccache-poc.yml`
  - artifact path: `build/windows-sccache-poc/windows-sccache-poc.json`
  - measurement intent:
    - compare same-run `without_sccache` vs `with_sccache` build pass timings
    - keep pass #1 cold within the current run when `reset_sccache_before_measurement=true`
    - record pass #2 `sccache` hit-rate from uploaded raw stats text
  - first captured workflow artifact:
    - run: `23245866965`
    - downloaded artifact: `build/windows-sccache-poc-gh-run-23245866965/windows-sccache-poc-23245866965/windows-sccache-poc.json`
    - baseline `without_sccache`:
      - pass #1 `93.91s`
      - pass #2 `94.78s`
    - `with_sccache`:
      - pass #1 `105.60s`
      - pass #2 `80.45s`
      - pass #2 hit rate `35.85%` (`19` hits / `34` misses)
  - decision:
    - no-go for immediate wider CI adoption
    - keep `.github/workflows/windows-sccache-poc.yml` as an optional comparison path only
    - rationale:
      - cold pass regressed by `12.45%`
      - warm pass improved by `15.12%`, but the hit rate stayed low enough that the gain does not yet justify wider rollout complexity
- frame the current Conan cache strategy so a future binary-remote run has one stable comparison surface
  - preferred workflow: `.github/workflows/conan2-poc.yml`
  - artifact path: `build/conan-strategy-poc/windows-conan-current-cache.json`
  - first captured baseline artifact:
    - run: `23246958472`
    - downloaded artifact: `build/conan-strategy-poc-gh-run-23246958472/conan-strategy-poc-windows-23246958472/windows-conan-current-cache.json`
    - target/config: `core_public_api_smoke`, `Release`
    - restore hit `true`, restore elapsed `15.51s`
    - build elapsed `1.26s`
    - ctest not executed
    - save skipped because of exact cache hit
  - decision:
    - current-cache baseline is now framed
    - no binary-remote rollout or adoption decision is made yet
    - any future binary-remote experiment should reuse the same workflow surface, target/config shape, and artifact fields before comparing wall clock or miss-recovery behavior
- collect additional `ci-prewarm` telemetry so current cache strategy stability is based on artifacts, not just step-summary text
  - preferred workflow: `.github/workflows/ci-prewarm.yml`
  - telemetry artifacts:
    - `build/ci-prewarm/windows-conan-prewarm.json`
    - `build/ci-prewarm/linux-base-image-prewarm.json`
  - additional captured telemetry run:
    - run: `23247349242`
    - downloaded artifacts:
      - `build/ci-prewarm-gh-run-23247349242/windows-conan-prewarm.json`
      - `build/ci-prewarm-gh-run-23247349242/linux-base-image-prewarm.json`
    - result:
      - Windows Conan restore exact hit `true`, restore elapsed `16.35s`, save skipped because of exact hit
      - Linux base-image build elapsed `24.58s`
  - decision:
    - current cache strategy still looks stable enough to keep using as-is
    - keep binary-remote work framed-only for now instead of promoting it into an active rollout candidate

## Related Docs

- `docs/tests.md`
- `docs/ops/realtime-runtime-contract.md`
- `docs/ops/session-continuity-contract.md`
- `docs/ops/mmorpg-world-residency-contract.md`
- `tools/loadgen/README.md`
