# tests

GTest unit tests + small Python integration checks.

## C++ (GTest)
- Definitions: `tests/CMakeLists.txt`
- Key targets:
  - `core_general_tests`, `core_concurrency_tests`
  - `server_general_tests`, `server_state_tests`
  - `chat_history_tests`, `storage_basic_tests`

Build (recommended on Windows):
```powershell
pwsh scripts/build.ps1 -Config Debug -Target server_general_tests
pwsh scripts/build.ps1 -Config Debug -Target chat_history_tests
```

Run:
```powershell
ctest --preset windows-test
```

Note: `ctest` does not rebuild binaries. If a test executable fails to start with an error like "entry point not found" (often after vcpkg updates or branch switches), rebuild the affected test target(s) or do a clean rebuild of `build-windows/`.

## Python
- `tests/python/verify_chat.py`
- `tests/python/verify_pong.py`
- `tests/python/test_load_balancing.py`
- `tests/python/verify_whisper_cross_instance.py`
- `tests/python/verify_admin_api.py`
- `tests/python/verify_admin_auth.py`
- `tests/python/verify_admin_control_plane_e2e.py`
- `tests/python/verify_runtime_toggle_metrics.py`
- `tests/python/test_check_doxygen_coverage.py`

Most Python tests expect the Docker stack (`docker/stack`) to be running.

Plugin/script smoke tests can be run through ctest labels after configuring tests:

```bash
cmake --preset linux -DBUILD_GTEST_TESTS=OFF -DBUILD_CONTRACT_TESTS=ON
KNIGHTS_ENABLE_STACK_PYTHON_TESTS=1 ctest --test-dir build-linux -L "plugin-script" --output-on-failure
```

Without `KNIGHTS_ENABLE_STACK_PYTHON_TESTS=1`, label-matched stack Python tests are skipped by design.
