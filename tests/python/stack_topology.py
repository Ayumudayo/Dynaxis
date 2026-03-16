from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
STACK_DIR = REPO_ROOT / "docker" / "stack"
DEFAULT_TOPOLOGY_CONFIG = STACK_DIR / "topologies" / "default.json"
ACTIVE_TOPOLOGY_PATH = STACK_DIR / "topology.active.json"
GENERATED_COMPOSE_PATH = STACK_DIR / "docker-compose.topology.generated.yml"
TOPOLOGY_ENV_VAR = "DYNAXIS_STACK_TOPOLOGY_CONFIG"
GENERATOR_SCRIPT = REPO_ROOT / "scripts" / "generate_stack_topology.py"


def resolve_repo_path(raw: str | Path) -> Path:
    candidate = Path(raw)
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


def resolve_topology_config_path() -> Path:
    raw = os.environ.get(TOPOLOGY_ENV_VAR, "").strip()
    if raw:
        return resolve_repo_path(raw)
    if ACTIVE_TOPOLOGY_PATH.exists():
        return ACTIVE_TOPOLOGY_PATH
    return DEFAULT_TOPOLOGY_CONFIG


def ensure_stack_topology_artifacts(topology_config: str | Path | None = None) -> dict:
    config_path = resolve_repo_path(topology_config) if topology_config else resolve_topology_config_path()
    command = [
        sys.executable,
        str(GENERATOR_SCRIPT),
        "--topology-config",
        str(config_path),
        "--output-compose",
        str(GENERATED_COMPOSE_PATH),
        "--output-active",
        str(ACTIVE_TOPOLOGY_PATH),
    ]
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"stack topology generation failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return load_stack_topology()


def load_stack_topology() -> dict:
    if not ACTIVE_TOPOLOGY_PATH.exists():
        ensure_stack_topology_artifacts()
    return json.loads(ACTIVE_TOPOLOGY_PATH.read_text(encoding="utf-8"))


def server_by_instance(topology: dict, instance_id: str) -> dict | None:
    for server in topology.get("servers", []):
        if isinstance(server, dict) and server.get("instance_id") == instance_id:
            return server
    return None


def servers_for_world(topology: dict, world_id: str) -> list[dict]:
    out: list[dict] = []
    for server in topology.get("servers", []):
        if isinstance(server, dict) and server.get("world_id") == world_id:
            out.append(server)
    return out


def same_world_peer(topology: dict, instance_id: str, world_id: str) -> dict | None:
    for server in servers_for_world(topology, world_id):
        if server.get("instance_id") != instance_id:
            return server
    return None


def first_server_for_other_world(topology: dict, excluded_world_id: str) -> dict | None:
    for server in topology.get("servers", []):
        if isinstance(server, dict) and server.get("world_id") != excluded_world_id:
            return server
    return None


def server_ready_ports(topology: dict) -> dict[str, int]:
    out: dict[str, int] = {}
    for server in topology.get("servers", []):
        if not isinstance(server, dict):
            continue
        instance_id = server.get("instance_id")
        metrics_host_port = server.get("metrics_host_port")
        if isinstance(instance_id, str) and isinstance(metrics_host_port, int):
            out[instance_id] = metrics_host_port
    return out


def server_metrics_ports(topology: dict) -> tuple[int, ...]:
    return tuple(server_ready_ports(topology).values())
