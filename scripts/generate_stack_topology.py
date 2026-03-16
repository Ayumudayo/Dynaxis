#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TOPOLOGY_CONFIG = REPO_ROOT / "docker" / "stack" / "topologies" / "default.json"
DEFAULT_OUTPUT_COMPOSE = REPO_ROOT / "docker" / "stack" / "docker-compose.topology.generated.yml"
DEFAULT_OUTPUT_ACTIVE = REPO_ROOT / "docker" / "stack" / "topology.active.json"


def resolve_repo_path(raw: str) -> Path:
    candidate = Path(raw)
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


def fail(message: str) -> None:
    raise ValueError(message)


def normalize_topology(data: object, source_path: Path) -> dict:
    if not isinstance(data, dict):
        fail("topology config root must be a JSON object")

    name = data.get("name")
    if name is None:
        name = source_path.stem
    if not isinstance(name, str) or not name.strip():
        fail("topology config `name` must be a non-empty string")

    servers = data.get("servers")
    if not isinstance(servers, list) or not servers:
        fail("topology config `servers` must be a non-empty array")

    normalized_servers: list[dict] = []
    seen_ids: set[str] = set()
    seen_host_ports: dict[int, str] = {}

    for index, raw_server in enumerate(servers, start=1):
        if not isinstance(raw_server, dict):
            fail(f"servers[{index}] must be an object")

        def read_required_string(key: str) -> str:
            value = raw_server.get(key)
            if not isinstance(value, str) or not value.strip():
                fail(f"servers[{index}].{key} must be a non-empty string")
            return value.strip()

        def read_required_port(key: str) -> int:
            value = raw_server.get(key)
            if not isinstance(value, int):
                fail(f"servers[{index}].{key} must be an integer")
            if value < 1 or value > 65535:
                fail(f"servers[{index}].{key} must be between 1 and 65535")
            return value

        instance_id = read_required_string("instance_id")
        world_id = read_required_string("world_id")
        shard = read_required_string("shard")
        tcp_host_port = read_required_port("tcp_host_port")
        metrics_host_port = read_required_port("metrics_host_port")

        extra_tags_raw = raw_server.get("extra_tags", [])
        if extra_tags_raw is None:
            extra_tags_raw = []
        if not isinstance(extra_tags_raw, list):
            fail(f"servers[{index}].extra_tags must be an array when provided")

        extra_tags: list[str] = []
        for tag_index, raw_tag in enumerate(extra_tags_raw, start=1):
            if not isinstance(raw_tag, str) or not raw_tag.strip():
                fail(f"servers[{index}].extra_tags[{tag_index}] must be a non-empty string")
            tag = raw_tag.strip()
            if "," in tag:
                fail(f"servers[{index}].extra_tags[{tag_index}] must not contain commas")
            if tag.startswith("world:"):
                fail(
                    f"servers[{index}].extra_tags[{tag_index}] must not override the generated world tag"
                )
            extra_tags.append(tag)

        if instance_id in seen_ids:
            fail(f"duplicate instance_id in topology config: {instance_id}")
        seen_ids.add(instance_id)

        for key, port in (("tcp_host_port", tcp_host_port), ("metrics_host_port", metrics_host_port)):
            prior = seen_host_ports.get(port)
            if prior is not None:
                fail(
                    f"duplicate host port in topology config: {port} "
                    f"used by {prior} and {instance_id}.{key}"
                )
            seen_host_ports[port] = f"{instance_id}.{key}"

        normalized_servers.append(
            {
                "instance_id": instance_id,
                "world_id": world_id,
                "shard": shard,
                "tcp_host_port": tcp_host_port,
                "metrics_host_port": metrics_host_port,
                "extra_tags": extra_tags,
            }
        )

    return {
        "name": name,
        "source_config": str(source_path),
        "servers": normalized_servers,
    }


def yaml_quote(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def render_server_service(server: dict) -> list[str]:
    tags = [f"world:{server['world_id']}", *server["extra_tags"]]
    tag_value = ",".join(tags)
    instance_id = server["instance_id"]
    shard = server["shard"]
    tcp_host_port = server["tcp_host_port"]
    metrics_host_port = server["metrics_host_port"]

    return [
        f"  {instance_id}:",
        "    image: dynaxis-server:local",
        "    pull_policy: never",
        "    build:",
        "      context: ../..",
        "      dockerfile: Dockerfile",
        "      target: server-runtime",
        "    command: [\"server\"]",
        "    environment:",
        "      PORT: \"5000\"",
        "      DB_URI: postgresql://${POSTGRES_USER:-dynaxis}:${POSTGRES_PASSWORD:-password}@postgres:5432/${POSTGRES_DB:-dynaxis_db}",
        "      REDIS_URI: tcp://redis:6379",
        "      USE_REDIS_PUBSUB: \"1\"",
        "      REDIS_CHANNEL_PREFIX: \"dynaxis:\"",
        "      WRITE_BEHIND_ENABLED: \"1\"",
        "      REDIS_STREAM_KEY: \"session_events\"",
        f"      SERVER_INSTANCE_ID: {yaml_quote(instance_id)}",
        f"      SERVER_ADVERTISE_HOST: {yaml_quote(instance_id)}",
        "      SERVER_ADVERTISE_PORT: \"5000\"",
        f"      SERVER_SHARD: {yaml_quote(shard)}",
        f"      SERVER_TAGS: {yaml_quote(tag_value)}",
        "      SERVER_REGISTRY_PREFIX: \"gateway/instances/\"",
        "      SERVER_REGISTRY_TTL: \"30\"",
        "      SERVER_HEARTBEAT_INTERVAL: \"5\"",
        f"      GATEWAY_ID: {yaml_quote(instance_id)}",
        "      METRICS_PORT: \"9090\"",
        "      RECENT_HISTORY_LIMIT: \"20\"",
        "      CHAT_HOOK_PLUGINS_DIR: \"/app/plugins\"",
        "      CHAT_HOOK_FALLBACK_PLUGINS_DIR: \"/app/plugins_builtin\"",
        "      CHAT_HOOK_ENABLED: \"${CHAT_HOOK_ENABLED:-0}\"",
        "      CHAT_HOOK_CACHE_DIR: \"/tmp/chat_hook_cache\"",
        "      CHAT_HOOK_RELOAD_INTERVAL_MS: \"500\"",
        "      LUA_ENABLED: \"${LUA_ENABLED:-0}\"",
        "      LUA_SCRIPTS_DIR: \"/app/scripts\"",
        "      LUA_FALLBACK_SCRIPTS_DIR: \"/app/scripts_builtin\"",
        "      SESSION_CONTINUITY_ENABLED: \"${SESSION_CONTINUITY_ENABLED:-0}\"",
        "      SESSION_CONTINUITY_LEASE_TTL_SEC: \"${SESSION_CONTINUITY_LEASE_TTL_SEC:-900}\"",
        "      ADMIN_COMMAND_SIGNING_SECRET: \"${ADMIN_COMMAND_SIGNING_SECRET:-dev-admin-command-secret}\"",
        "      ADMIN_COMMAND_TTL_MS: \"${ADMIN_COMMAND_TTL_MS:-60000}\"",
        "      ADMIN_COMMAND_FUTURE_SKEW_MS: \"${ADMIN_COMMAND_FUTURE_SKEW_MS:-5000}\"",
        "      SERVER_DRAIN_TIMEOUT_MS: \"${SERVER_DRAIN_TIMEOUT_MS:-15000}\"",
        "      SERVER_DRAIN_POLL_MS: \"${SERVER_DRAIN_POLL_MS:-100}\"",
        "      RUNTIME_TRACING_ENABLED: \"${RUNTIME_TRACING_ENABLED:-0}\"",
        "      RUNTIME_TRACING_SAMPLE_PERCENT: \"${RUNTIME_TRACING_SAMPLE_PERCENT:-100}\"",
        "    volumes:",
        "      - ./plugins:/app/plugins:ro",
        "      - ./scripts:/app/scripts:ro",
        "    depends_on:",
        "      redis:",
        "        condition: service_healthy",
        "      postgres:",
        "        condition: service_healthy",
        "      migrator:",
        "        condition: service_completed_successfully",
        "    networks:",
        "      - dynaxis-stack",
        "    ports:",
        f"      - \"{tcp_host_port}:5000\"",
        f"      - \"{metrics_host_port}:9090\"",
    ]


def render_compose(topology: dict) -> str:
    lines = [
        "# Generated by scripts/generate_stack_topology.py. Do not edit manually.",
        f"# Source topology: {topology['source_config']}",
        "",
        "services:",
    ]
    for server in topology["servers"]:
        lines.extend(render_server_service(server))
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate docker stack server topology compose.")
    parser.add_argument(
        "--topology-config",
        default=str(DEFAULT_TOPOLOGY_CONFIG),
        help="Path to topology JSON config.",
    )
    parser.add_argument(
        "--output-compose",
        default=str(DEFAULT_OUTPUT_COMPOSE),
        help="Path to generated compose YAML.",
    )
    parser.add_argument(
        "--output-active",
        default=str(DEFAULT_OUTPUT_ACTIVE),
        help="Path to generated normalized active topology JSON.",
    )
    args = parser.parse_args()

    try:
        topology_config_path = resolve_repo_path(args.topology_config)
        if not topology_config_path.exists():
            fail(f"topology config not found: {topology_config_path}")

        raw_topology = json.loads(topology_config_path.read_text(encoding="utf-8"))
        topology = normalize_topology(raw_topology, topology_config_path)

        output_compose_path = resolve_repo_path(args.output_compose)
        output_active_path = resolve_repo_path(args.output_active)

        write_text(output_compose_path, render_compose(topology))
        write_json(output_active_path, topology)

        print(f"generated compose: {output_compose_path}")
        print(f"generated topology snapshot: {output_active_path}")
        return 0
    except Exception as exc:
        print(f"[topology-generator] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
