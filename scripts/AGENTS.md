# scripts

Canonical entry points for building and running the stack.

## Key Scripts
- `scripts/build.ps1`: CMake preset configure/build; `-Target <name>` builds a single target.
- `scripts/configure_windows_ninja.ps1`: configures `windows-ninja` preset (for clangd/`compile_commands.json`).
- `scripts/check_observability.ps1`: sanity-check Prometheus targets (and Grafana health).
- `scripts/deploy_docker.ps1`: manage `docker/stack` compose (`up/down/restart/build/logs/ps/clean/config`) with optional `-EnvFile` override.
- `scripts/run_full_stack_observability.ps1`: wrapper for `deploy_docker.ps1 -Observability`.
- `scripts/smoke_wb.ps1`: end-to-end smoke test for the write-behind pipeline (expects the Docker stack up).
- `scripts/rehearse_udp_rollout_rollback.ps1`: execute UDP canary -> TCP-only rollback rehearsal and verify completion within 10 minutes.

## Examples
```powershell
pwsh scripts/build.ps1 -Config Debug -Target server_app
pwsh scripts/configure_windows_ninja.ps1 -CopyCompileCommands
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability
```

## Related Checks
- Opcode spec/doc check (CI uses this): `python tools/gen_opcode_docs.py --check`
- Sanitizers (Linux): `cmake --preset linux-asan` (sets `KNIGHTS_ENABLE_SANITIZERS=ON`)
