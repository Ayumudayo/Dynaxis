#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import cast


REPO_ROOT = Path(__file__).resolve().parents[2]
STACK_DIR = REPO_ROOT / "docker" / "stack"
ACTIVE_TOPOLOGY_PATH = STACK_DIR / "topology.active.json"

SERVER_CONTAINERS = (
    "dynaxis-stack-server-1-1",
    "dynaxis-stack-server-2-1",
)
GATEWAY1_CONTAINER = "dynaxis-stack-gateway-1-1"


def _is_enabled() -> bool:
    value = os.environ.get("ENABLE_STACK_PYTHON_TESTS", "")
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _is_truthy(value: str | None) -> bool:
    if value is None:
        return False
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _find_option(args: list[str], name: str) -> str | None:
    for index, item in enumerate(args):
        if item != name:
            continue
        next_index = index + 1
        if next_index >= len(args):
            return None
        return args[next_index]
    return None


def _docker_inspect_env(container: str) -> dict[str, str] | None:
    result = subprocess.run(
        [
            "docker",
            "inspect",
            "--format",
            "{{json .Config.Env}}",
            container,
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None

    raw = result.stdout.strip()
    if not raw:
        return {}

    values = json.loads(raw)
    env_map: dict[str, str] = {}
    for item in values:
        if not isinstance(item, str) or "=" not in item:
            continue
        key, value = item.split("=", 1)
        env_map[key] = value
    return env_map


def _http_ready(port: int) -> bool:
    url = f"http://127.0.0.1:{port}/readyz"
    try:
        with urllib.request.urlopen(url, timeout=2.0) as response:
            body = response.read().decode("utf-8", errors="replace").strip()
            return response.status == 200 and body == "ready"
    except urllib.error.HTTPError:
        return False
    except Exception:
        return False


def _tcp_ready(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1.0):
            return True
    except OSError:
        return False


def _load_active_topology() -> dict | None:
    if not ACTIVE_TOPOLOGY_PATH.exists():
        return None
    try:
        return json.loads(ACTIVE_TOPOLOGY_PATH.read_text(encoding="utf-8"))
    except Exception:
        return None


def _find_server(topology: dict, instance_id: str) -> dict | None:
    for server in topology.get("servers", []):
        if isinstance(server, dict) and server.get("instance_id") == instance_id:
            return server
    return None


def _has_same_world_peer(topology: dict, instance_id: str, world_id: str) -> bool:
    for server in topology.get("servers", []):
        if not isinstance(server, dict):
            continue
        if server.get("instance_id") == instance_id:
            continue
        if server.get("world_id") == world_id:
            return True
    return False


def _require_server_runtime_on() -> str | None:
    missing: list[str] = []
    for container in SERVER_CONTAINERS:
        env_map = _docker_inspect_env(container)
        if env_map is None:
            missing.append(container)
            continue
        if env_map.get("CHAT_HOOK_ENABLED") != "1" or env_map.get("LUA_ENABLED") != "1":
            return (
                "current stack is not runtime-on for plugin/script proofs: "
                f"{container} requires CHAT_HOOK_ENABLED=1 and LUA_ENABLED=1"
            )
    if missing:
        return "required server containers are not running: " + ", ".join(missing)
    if not _http_ready(39091) or not _http_ready(39092) or not _tcp_ready(6000):
        return "plugin/script runtime-on stack is not ready on 39091/39092/6000"
    return None


def _require_fps_direct_udp() -> str | None:
    env_map = _docker_inspect_env(GATEWAY1_CONTAINER)
    if env_map is None:
        return f"required gateway container is not running: {GATEWAY1_CONTAINER}"
    if not env_map.get("GATEWAY_UDP_LISTEN", "").strip():
        return "current stack does not expose direct UDP ingress (GATEWAY_UDP_LISTEN is empty)"
    if not env_map.get("GATEWAY_UDP_BIND_SECRET", "").strip():
        return "current stack does not expose UDP bind tickets (GATEWAY_UDP_BIND_SECRET is empty)"
    allowlist = env_map.get("GATEWAY_UDP_OPCODE_ALLOWLIST", "")
    if "0x0206" not in allowlist:
        return "current stack does not allow FPS UDP opcode 0x0206 on the direct UDP path"
    backend_env = _docker_inspect_env("dynaxis-stack-server-1-1")
    if backend_env is None:
        return "direct FPS transport requires at least one running server backend container"
    if not _http_ready(36001) or not _tcp_ready(36100):
        return "direct UDP gateway-1 path is not ready on 36001/36100"
    if not _http_ready(39091):
        return "direct FPS transport backend server is not ready on 39091"
    return None


def _require_fps_direct_rudp() -> str | None:
    reason = _require_fps_direct_udp()
    if reason is not None:
        return reason
    env_map = _docker_inspect_env(GATEWAY1_CONTAINER)
    assert env_map is not None
    if env_map.get("GATEWAY_RUDP_ENABLE") != "1":
        return "current stack does not enable the RUDP direct path (GATEWAY_RUDP_ENABLE != 1)"
    try:
        canary_percent = int(env_map.get("GATEWAY_RUDP_CANARY_PERCENT", "0"))
    except ValueError:
        canary_percent = 0
    if canary_percent <= 0:
        return "current stack does not route traffic into the RUDP canary path"
    allowlist = env_map.get("GATEWAY_RUDP_OPCODE_ALLOWLIST", "")
    if "0x0206" not in allowlist:
        return "current stack does not allow FPS opcode 0x0206 on the RUDP path"
    return None


def _require_continuity_default() -> str | None:
    env_map = _docker_inspect_env("dynaxis-stack-server-1-1")
    if env_map is None:
        return "continuity proofs require dynaxis-stack server containers to be running"
    if env_map.get("SESSION_CONTINUITY_ENABLED") != "1":
        return "current stack does not enable SESSION_CONTINUITY_ENABLED=1"

    topology = _load_active_topology()
    if topology is None:
        return f"active topology snapshot is missing: {ACTIVE_TOPOLOGY_PATH}"

    for port in (39200, 36001, 36002):
        if not _http_ready(port):
            return f"continuity control plane is not ready on /readyz port {port}"
    for port in (36100, 36101):
        if not _tcp_ready(port):
            return f"continuity gateway tcp path is not ready on port {port}"

    for server in topology.get("servers", []):
        if not isinstance(server, dict):
            continue
        metrics_host_port = server.get("metrics_host_port")
        if isinstance(metrics_host_port, int) and not _http_ready(metrics_host_port):
            return f"server metrics/ready path is not ready on port {metrics_host_port}"
    return None


def _require_continuity_same_world() -> str | None:
    reason = _require_continuity_default()
    if reason is not None:
        return reason
    topology = _load_active_topology()
    assert topology is not None
    primary_server = _find_server(topology, "server-1")
    if primary_server is None:
        return "active topology does not define server-1"
    primary_world_id = str(primary_server.get("world_id", ""))
    if not primary_world_id:
        return "active topology server-1 is missing world_id"
    if not _has_same_world_peer(topology, "server-1", primary_world_id):
        return (
            "active topology does not provide a same-world peer for same-world closure proofs "
            f"(server-1 world_id={primary_world_id})"
        )
    return None


def _infer_prerequisite_reason(script_path: Path, script_args: list[str]) -> str | None:
    script_name = script_path.name

    if script_name in {
        "verify_plugin_hot_reload.py",
        "verify_plugin_v2_fallback.py",
        "verify_plugin_rollback.py",
        "verify_script_hot_reload.py",
        "verify_script_fallback_switch.py",
        "verify_chat_hook_behavior.py",
    }:
        return _require_server_runtime_on()

    if script_name == "verify_runtime_toggle_metrics.py":
        expect_chat_hook = _find_option(script_args, "--expect-chat-hook-enabled")
        expect_lua = _find_option(script_args, "--expect-lua-enabled")
        if expect_chat_hook == "1" or expect_lua == "1":
            return _require_server_runtime_on()
        return None

    if script_name == "verify_fps_state_transport.py":
        return _require_fps_direct_udp()

    if script_name == "verify_fps_rudp_transport.py":
        scenario = _find_option(script_args, "--scenario") or "attach"
        if scenario == "attach":
            return _require_fps_direct_rudp()
        return None

    if script_name == "verify_fps_netem_rehearsal.py":
        if not _is_truthy(os.environ.get("ENABLE_STACK_NETEM_TESTS")):
            return "manual ops-only netem rehearsal requires ENABLE_STACK_NETEM_TESTS=1"
        return None

    if script_name == "verify_session_continuity_restart.py":
        scenario = _find_option(script_args, "--scenario") or ""
        if scenario in {
            "world-drain-progress",
            "world-drain-migration-closure",
            "world-migration-handoff",
            "world-migration-target-room-handoff",
        }:
            return _require_continuity_default()
        if scenario in {
            "world-drain-transfer-closure",
            "world-owner-transfer-commit",
        }:
            return _require_continuity_same_world()
        return None

    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run a stack-dependent Python test script for ctest. "
            "Returns 77 when ENABLE_STACK_PYTHON_TESTS is not enabled."
        )
    )
    _ = parser.add_argument("script", help="Python test script path")
    _ = parser.add_argument(
        "script_args", nargs=argparse.REMAINDER, help="Arguments for the script"
    )
    args = parser.parse_args()
    script = cast(str, args.script)
    script_args = cast(list[str], args.script_args)

    if not _is_enabled():
        print("[skip] ENABLE_STACK_PYTHON_TESTS is not enabled.")
        return 77

    script_path = Path(script)
    if not script_path.exists():
        print(f"[error] script not found: {script_path}")
        return 2

    prerequisite_reason = _infer_prerequisite_reason(script_path, script_args)
    if prerequisite_reason is not None:
        print(f"[skip] {prerequisite_reason}")
        return 77

    command = [sys.executable, str(script_path), *script_args]
    completed = subprocess.run(command, check=False)
    return int(completed.returncode)


if __name__ == "__main__":
    raise SystemExit(main())
