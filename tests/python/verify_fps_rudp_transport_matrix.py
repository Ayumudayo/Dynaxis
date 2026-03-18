from __future__ import annotations

import argparse
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
VERIFY_SCRIPT = Path(__file__).resolve().with_name("verify_fps_rudp_transport.py")
ATTACH_ENV_FILE = REPO_ROOT / "docker" / "stack" / ".env.rudp-attach.example"
OFF_ENV_FILE = REPO_ROOT / "docker" / "stack" / ".env.rudp-off.example"
ROLLOUT_FALLBACK_ENV_FILE = REPO_ROOT / "docker" / "stack" / ".env.rudp-fallback.example"

GATEWAY_METRICS_PORTS = (36001, 36002)
DIRECT_GATEWAY_HOST = "127.0.0.1"
DIRECT_GATEWAY_PORT = 36100


def run_command(command: list[str], *, label: str) -> None:
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"{label} failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if result.stdout.strip():
        print(result.stdout.strip())


def invoke_deploy(action: str, env_file: Path, *, detached: bool, build: bool) -> None:
    command = [
        "pwsh",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(DEPLOY_SCRIPT),
        "-Action",
        action,
        "-EnvFile",
        str(env_file),
    ]
    if detached:
        command.append("-Detached")
    if build:
        command.append("-Build")
    run_command(command, label=f"deploy:{action}")


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


def wait_stack_ready(timeout_sec: float) -> None:
    topology = ensure_stack_topology_artifacts()
    for port in GATEWAY_METRICS_PORTS:
        wait_http_ready(port, timeout_sec)
    for port in server_ready_ports(topology).values():
        wait_http_ready(port, timeout_sec)
    wait_tcp_ready(DIRECT_GATEWAY_HOST, DIRECT_GATEWAY_PORT, timeout_sec)


def run_verify(scenario: str, env_file: Path) -> None:
    command = [
        sys.executable,
        str(VERIFY_SCRIPT),
        "--scenario",
        scenario,
        "--compose-env-file",
        str(env_file),
    ]
    run_command(command, label=f"verify:{scenario}")


def run_stage(name: str, env_file: Path, scenario: str, *, build: bool, timeout_sec: float) -> None:
    print(f"[stage] {name}")
    invoke_deploy("down", env_file, detached=False, build=False)
    invoke_deploy("up", env_file, detached=True, build=build)
    wait_stack_ready(timeout_sec)
    run_verify(scenario, env_file)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the Docker-backed Phase 2 FPS acceptance matrix across attach/off/fallback/restart/impaired-network stages"
    )
    parser.add_argument(
        "--scenario",
        choices=(
            "phase2-acceptance",
            "matrix",
            "attach",
            "off",
            "rollout-fallback",
            "protocol-fallback",
            "udp-quality-impairment",
            "restart",
        ),
        default="phase2-acceptance",
    )
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--stack-ready-timeout-sec", type=float, default=90.0)
    args = parser.parse_args()

    stage_map = {
        "attach": (ATTACH_ENV_FILE, "attach"),
        "off": (OFF_ENV_FILE, "off"),
        "rollout-fallback": (ROLLOUT_FALLBACK_ENV_FILE, "rollout-fallback"),
        "protocol-fallback": (ATTACH_ENV_FILE, "protocol-fallback"),
        "udp-quality-impairment": (OFF_ENV_FILE, "udp-quality-impairment"),
        "restart": (ATTACH_ENV_FILE, "restart"),
    }

    if args.scenario in {"phase2-acceptance", "matrix"}:
        plan = [
            ("rudp-attach", ATTACH_ENV_FILE, "attach"),
            ("udp-only-off", OFF_ENV_FILE, "off"),
            ("rollout-fallback", ROLLOUT_FALLBACK_ENV_FILE, "rollout-fallback"),
            ("protocol-fallback", ATTACH_ENV_FILE, "protocol-fallback"),
            ("udp-quality-impairment", OFF_ENV_FILE, "udp-quality-impairment"),
            ("rudp-restart", ATTACH_ENV_FILE, "restart"),
        ]
    else:
        env_file, scenario = stage_map[args.scenario]
        plan = [(args.scenario, env_file, scenario)]

    last_env_file = ATTACH_ENV_FILE
    build = not args.no_build
    try:
        for name, env_file, scenario in plan:
            last_env_file = env_file
            run_stage(
                name,
                env_file,
                scenario,
                build=build,
                timeout_sec=args.stack_ready_timeout_sec,
            )
            build = False
        print("PASS phase2-acceptance: all requested FPS transport stages succeeded")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: {exc}")
        return 1
    finally:
        try:
            invoke_deploy("down", last_env_file, detached=False, build=False)
        except Exception as exc:  # noqa: BLE001
            print(f"[warn] stack cleanup failed: {exc}")


if __name__ == "__main__":
    raise SystemExit(main())
