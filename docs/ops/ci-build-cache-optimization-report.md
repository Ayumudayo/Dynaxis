# CI Build Cache Optimization Report (Conan2)

## 1) Goal

- Minimize Windows CI wall-clock time while keeping required checks stable.
- Use a single dependency/cache strategy across required Windows jobs.

## 2) Current Architecture

### Workflow structure

- Required workflow: `.github/workflows/ci.yml`
- Windows dependency priming job:
  - `windows-conan-deps`
  - restores Conan cache
  - installs missing dependency graph when cache miss occurs
  - saves cache for downstream jobs
- Dependent Windows jobs:
  - `core-api-consumer-windows`
  - `windows-fast-tests`

### Cache policy

- Cache location: `${{ github.workspace }}/../.cache/conan2`
- Key inputs:
  - `conan.lock`
  - `conanfile.py`
  - `conan/profiles/**`
  - `scripts/setup_conan.ps1`
  - `scripts/build.ps1`
- Telemetry (step summary):
  - restore hit/matched key/elapsed
  - save status/elapsed (skip on exact hit)

## 3) What Is Reused

- Conan package/binary cache across runs.
- Dependency resolution artifacts under `CONAN_HOME`.

## 4) What Is Not Reused

- C++ compile outputs (`.obj`, `.lib`, `.exe`) are still rebuilt each run.
- This is expected with ephemeral hosted runners unless explicit build artifact promotion is added.

## 5) Primary Cost Drivers

- Build step dominates Windows runtime in most runs.
- Cold cache + `--build=missing` expands first-run time significantly.
- Two Windows jobs can still both pay compile cost even when dependency cache is warm.

## 6) Optimization Backlog

1. Cache key stability hardening
- Keep key inputs narrowly scoped to dependency graph changes.
- Avoid volatile/non-dependency files in cache key composition.

2. Build surface reduction in required jobs
- Ensure `windows-fast-tests` compiles only required targets.
- Keep API contract checks and unit tests in minimal dependency order.

3. Optional compile cache expansion
- Promote `windows-sccache-poc.yml` learnings into required path when hit rate and reliability are proven.

4. Runtime trend tracking
- Keep periodic baselines for:
  - total Windows job time
  - build step time
  - cache restore/save times

## 7) Operational Checks

Use these checks after workflow changes:

```bash
gh run list --workflow "ci.yml" --limit 10
gh run view <run-id> --json jobs
```

Validate that:

- `windows-conan-deps` completes before dependent Windows jobs.
- dependent Windows jobs show cache restore hit/fallback as expected.
- no dependency-setup duplication reappears in multiple jobs.

## 8) Success Criteria

- Required checks remain green.
- Median Windows CI wall-clock remains stable or improves after changes.
- Cold run behavior is bounded; warm run variance stays low.
