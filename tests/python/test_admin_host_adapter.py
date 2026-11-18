#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import admin_host_adapter as adapter  # noqa: E402


def assert_raises_value_error(fn, *args) -> None:
    try:
        fn(*args)
    except ValueError:
        return
    raise AssertionError(f"Expected ValueError for {args!r}")


def main() -> int:
    cmds, env_map = adapter.build_execution_commands(
        {
            "operation_type": "service_restart",
            "target_service": "server-1",
            "params": {"affected_services": ["server-1"]},
        }
    )
    assert cmds == [["pwsh", "scripts/deploy_docker.ps1", "-Action", "restart", "-Service", "server-1"]], cmds
    assert env_map == {}, env_map

    cfg_cmds, cfg_env = adapter.build_execution_commands(
        {
            "operation_type": "config_apply",
            "target_service": "stack-config",
            "params": {
                "affected_services": ["server-1", "server-2", "wb_worker"],
                "managed_env": {
                    "SESSION_CONTINUITY_ENABLED": "1",
                    "SESSION_CONTINUITY_LEASE_TTL_SEC": "900",
                    "SERVER_DRAIN_TIMEOUT_MS": "15000",
                    "RUNTIME_TRACING_ENABLED": "0",
                    "RUNTIME_TRACING_SAMPLE_PERCENT": "100",
                },
            },
        }
    )
    assert cfg_cmds == [
        [
            "pwsh",
            "scripts/deploy_docker.ps1",
            "-Action",
            "up",
            "-Detached",
            "-EnvFile",
            "docker/stack/.env.managed",
            "-Service",
            svc,
        ]
        for svc in ("server-1", "server-2", "wb_worker")
    ], cfg_cmds
    assert cfg_env["SESSION_CONTINUITY_ENABLED"] == "1"

    for bad_svc in ("postgres", "redis", "haproxy", "admin", "server-1;rm", "../server-1", "gateway-0"):
        assert_raises_value_error(
            adapter.build_execution_commands,
            {"operation_type": "service_restart", "target_service": bad_svc},
        )

    assert_raises_value_error(
        adapter.build_execution_commands,
        {
            "operation_type": "config_apply",
            "target_service": "stack-config",
            "params": {
                "affected_services": ["server-1"],
                "managed_env": {"CHAT_AUTH_SIGNING_SECRET": "abc"},
            },
        },
    )

    assert_raises_value_error(
        adapter.build_execution_commands,
        {
            "operation_type": "config_apply",
            "target_service": "stack-config",
            "params": {
                "affected_services": ["server-1"],
                "managed_env": {"SERVER_DRAIN_TIMEOUT_MS": "15000; whoami"},
            },
        },
    )

    sanitized = adapter.sanitize_output(
        "failed postgresql://dynaxis:super_pw@db:5432/app token=my_tok_123",
        secret_token="my_tok_123",
    )
    assert "super_pw" not in sanitized and "my_tok_123" not in sanitized, sanitized

    print("PASS: admin_host_adapter allowlists, argv builders, and redaction checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
