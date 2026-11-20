# tests

GTest unit tests + small Python integration checks.

## C++ (GTest)
- Definitions: `tests/CMakeLists.txt`, `tests/core/CMakeLists.txt`, `tests/gateway/CMakeLists.txt`, `tests/server/CMakeLists.txt`, `tests/admin/CMakeLists.txt`, `tests/contracts/CMakeLists.txt`, `tests/policy/CMakeLists.txt`
- Key targets:
  - `core_general_tests`, `core_concurrency_tests`
  - `server_general_tests`, `server_state_tests`
  - `chat_history_tests`, `storage_basic_tests`, `chat_timeline_tests`
  - `admin_console_tests` (`tests/admin/`, bounded probes, poller isolation, auth/redaction)

Build & run (Windows / Linux):
```powershell
python scripts/build.py --profile full --config Debug --step all
python scripts/build.py --profile full --config Debug --step test
```

Note: `ctest` does not rebuild binaries. If dependencies changed, rerun `python scripts/build.py --profile full --config Debug --step deps` followed by `configure` and `build`.

## Python
- `tests/python/verify_signed_chat.py --manage-stack`: signed identity, direct-backend rejection, cross-node delivery and resume binding; restores original deployment environment.
- `tests/python/verify_core_dependency_recovery.py`: real Postgres/Redis outages and readiness/persistence/fanout recovery without app restart (local development stack only).
- `tests/python/verify_chat.py`
- `tests/python/verify_pong.py`
- `tests/python/verify_whisper_cross_instance.py`
- `tests/python/verify_runtime_toggle_metrics.py`
- `tests/python/test_check_doxygen_coverage.py`

Most Python tests expect the Docker stack (`docker/stack`) to be running.


```bash
cmake --preset linux -DBUILD_GTEST_TESTS=OFF -DBUILD_CONTRACT_TESTS=ON
```

Without `ENABLE_STACK_PYTHON_TESTS=1`, label-matched stack Python tests are skipped by design.
