from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from stack_topology import ensure_stack_topology_artifacts
from stack_topology import server_ready_ports


REPO_ROOT = Path(__file__).resolve().parents[2]
DEPLOY_SCRIPT = REPO_ROOT / "scripts" / "deploy_docker.ps1"
DEFAULT_TOPOLOGY = REPO_ROOT / "docker" / "stack" / "topologies" / "default.json"
SAME_WORLD_TOPOLOGY = REPO_ROOT / "docker" / "stack" / "topologies" / "mmorpg-same-world-proof.json"

ADMIN_API_SCRIPT = Path(__file__).resolve().with_name("verify_admin_api.py")
ADMIN_AUTH_SCRIPT = Path(__file__).resolve().with_name("verify_admin_auth.py")
ADMIN_READ_ONLY_SCRIPT = Path(__file__).resolve().with_name("verify_admin_read_only.py")
CONTINUITY_SCRIPT = Path(__file__).resolve().with_name("verify_session_continuity_restart.py")

ADMIN_READY_PORT = 39200
GATEWAY_METRICS_PORTS = (36001, 36002)
DIRECT_GATEWAY_PORTS = (36100, 36101)


def run_command(command: list[str], *, label: str, extra_env: dict[str, str] | None = None) -> None:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        text=True,
        check=False,
        env=env,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"{label} failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if result.stdout.strip():
        print(result.stdout.strip())


def invoke_deploy(action: str, topology_config: Path, *, detached: bool, build: bool) -> None:
    command = [
        "pwsh",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(DEPLOY_SCRIPT),
        "-Action",
        action,
        "-TopologyConfig",
        str(topology_config),
    ]
    if detached:
        command.append("-Detached")
    if build:
        command.append("-Build")
    run_command(command, label=f"deploy:{action}", extra_env={"SESSION_CONTINUITY_ENABLED": "1"})


def wait_http_ready(port: int, timeout_sec: float) -> None:
    url = f"http://127.0.0.1:{port}/readyz"
    deadline = time.monotonic() + timeout_sec
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=3.0) as response:
                body = response.read().decode("utf-8", errors="replace").strip()
                if response.status == 200 and body == "ready":
                    return
                last_error = f"status={response.status} body={body!r}"
        except urllib.error.HTTPError as exc:
            last_error = f"status={exc.code} body={exc.read().decode('utf-8', errors='replace')!r}"
        except Exception as exc:  # noqa: BLE001
            last_error = str(exc)
        time.sleep(0.5)
    raise TimeoutError(f"readyz timeout on {url}: {last_error}")


def wait_tcp_ready(host: str, port: int, timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return
        except OSError as exc:
            last_error = str(exc)
        time.sleep(0.5)
    raise TimeoutError(f"tcp timeout on {host}:{port}: {last_error}")


def wait_stack_ready(topology_config: Path, timeout_sec: float) -> None:
    topology = ensure_stack_topology_artifacts(topology_config)
    wait_http_ready(ADMIN_READY_PORT, timeout_sec)
    for port in GATEWAY_METRICS_PORTS:
        wait_http_ready(port, timeout_sec)
    for port in server_ready_ports(topology).values():
        wait_http_ready(port, timeout_sec)
    for port in DIRECT_GATEWAY_PORTS:
        wait_tcp_ready("127.0.0.1", port, timeout_sec)


def run_python_script(script: Path, *script_args: str) -> None:
    command = [sys.executable, str(script), *script_args]
    label = f"python:{script.name}"
    if script_args:
        label = f"{label}:{' '.join(script_args)}"
    run_command(command, label=label)


def run_stage(name: str, topology_config: Path, commands: list[list[str]], *, build: bool, timeout_sec: float) -> None:
    print(f"[stage] {name}")
    invoke_deploy("down", topology_config, detached=False, build=False)
    invoke_deploy("up", topology_config, detached=True, build=build)
    wait_stack_ready(topology_config, timeout_sec)
    for command in commands:
        run_python_script(Path(command[0]), *command[1:])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run the topology-aware MMORPG Phase 3 acceptance matrix across default and same-world stack manifests."
        )
    )
    parser.add_argument(
        "--scenario",
        choices=("phase3-acceptance", "matrix", "default-closure", "same-world-closure"),
        default="phase3-acceptance",
    )
    parser.add_argument(
        "--default-topology",
        type=Path,
        default=DEFAULT_TOPOLOGY,
    )
    parser.add_argument(
        "--same-world-topology",
        type=Path,
        default=SAME_WORLD_TOPOLOGY,
    )
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--stack-ready-timeout-sec", type=float, default=90.0)
    args = parser.parse_args()

    stage_map = {
        "default-closure": (
            args.default_topology,
            [
                [str(ADMIN_API_SCRIPT)],
                [str(ADMIN_AUTH_SCRIPT)],
                [str(ADMIN_READ_ONLY_SCRIPT)],
                [str(CONTINUITY_SCRIPT), "--scenario", "world-drain-progress"],
                [str(CONTINUITY_SCRIPT), "--scenario", "world-drain-migration-closure"],
                [str(CONTINUITY_SCRIPT), "--scenario", "world-migration-handoff"],
                [str(CONTINUITY_SCRIPT), "--scenario", "world-migration-target-room-handoff"],
            ],
        ),
        "same-world-closure": (
            args.same_world_topology,
            [
                [str(CONTINUITY_SCRIPT), "--scenario", "world-owner-transfer-commit"],
                [str(CONTINUITY_SCRIPT), "--scenario", "world-drain-transfer-closure"],
            ],
        ),
    }

    if args.scenario in {"phase3-acceptance", "matrix"}:
        plan = [
            ("default-topology-control-plane-live-scale-and-cross-world-closure", *stage_map["default-closure"]),
            ("same-world-owner-transfer-and-drain-closure", *stage_map["same-world-closure"]),
        ]
    else:
        topology_config, commands = stage_map[args.scenario]
        plan = [(args.scenario, topology_config, commands)]

    last_topology = DEFAULT_TOPOLOGY
    build = not args.no_build
    try:
        for stage_name, topology_config, commands in plan:
            last_topology = topology_config
            run_stage(
                stage_name,
                topology_config,
                commands,
                build=build,
                timeout_sec=args.stack_ready_timeout_sec,
            )
            build = False
        print("PASS phase3-acceptance: all requested MMORPG runtime closure stages succeeded")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: {exc}")
        return 1
    finally:
        try:
            invoke_deploy("down", last_topology, detached=False, build=False)
        except Exception as exc:  # noqa: BLE001
            print(f"[warn] stack cleanup failed: {exc}")


if __name__ == "__main__":
    raise SystemExit(main())
