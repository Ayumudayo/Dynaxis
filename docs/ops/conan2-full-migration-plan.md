# Conan2 Cutover Status (Completed)

## 1) Scope

- This repository now uses Conan2 as the single dependency provider for local Windows builds and required Windows CI jobs.
- Build presets and scripts no longer require legacy package-manager toolchains or manifests.
- Runtime DLL handling on Windows uses target metadata (`TARGET_RUNTIME_DLLS`) and shared helper scripts.

## 2) Completed Changes

### Build entry points

- `scripts/build.ps1`
  - Conan mode is now the default path.
  - Legacy provider switches/bootstrap paths are removed from execution flow.
  - Default Windows build directories:
    - full: `build-windows`
    - client-only: `build-windows-client`

- `scripts/setup_conan.ps1`
  - Default Windows build directories now align with standard presets (`build-windows`, `build-windows-client`).

- `scripts/configure_windows_ninja.ps1`
  - Runs Conan setup first, then configures `windows-ninja` presets.

### CMake presets

- `CMakePresets.json`
  - Windows default presets (`windows`, `windows-client`, `windows-ninja*`) use Conan toolchain paths.
  - Conan compatibility presets remain available for CI/automation continuity.
  - `windows-test` now points to Conan-backed configure state through `windows` preset.

### CMake runtime handling

- `CMakeLists.txt`
  - Removed hardwired Windows runtime directory copies from legacy provider output folders.
- `server/CMakeLists.txt`
  - Removed provider-specific DLL copy block; keeps metadata-driven runtime DLL copy only.

### Removed repository artifacts

- `vcpkg.json`
- `cmake/vcpkg_toolchain.cmake`
- `scripts/setup_vcpkg.ps1`
- `scripts/setup_vcpkg.sh`

## 3) Current Operational Rules

- Local Windows build:

```powershell
pwsh scripts/build.ps1 -Config Debug
ctest --preset windows-test --output-on-failure
```

- Local Windows client-only build:

```powershell
pwsh scripts/build.ps1 -ClientOnly -Target client_gui
```

- Linux Conan build (when not using Docker runtime flow):

```powershell
pwsh scripts/build.ps1 -Config Debug
```

- Docker runtime/integration tests remain the standard Linux runtime validation path:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
python tests/python/verify_pong.py
pwsh scripts/deploy_docker.ps1 -Action down
```

## 4) CI Contract

- Required Windows jobs run Conan cache restore/prime and Conan-based build/test flows.
- Windows dependency priming is centralized in a dedicated job in the same workflow run and reused by dependent Windows jobs.
- Required check names are preserved for branch protection stability.

## 5) Verification Checklist

- `git grep -n "setup_vcpkg|vcpkg_toolchain|VCPKG_|vcpkg.json" -- . ":(exclude)docs/**"` returns no matches.
- Windows default commands (`scripts/build.ps1 -Config Debug`, `ctest --preset windows-test`) succeed with Conan-generated toolchain.
- Required CI passes with Conan cache telemetry present.

## 6) Follow-up Guardrails

- Do not reintroduce provider-specific path copies in CMake post-build commands.
- Keep lockfiles (`conan.lock`, `conan-client.lock`) updated with intentional dependency changes only.
- Preserve cache-key stability in CI by hashing only dependency-relevant inputs.
