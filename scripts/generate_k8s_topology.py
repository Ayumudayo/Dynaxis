#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from generate_stack_topology import DEFAULT_TOPOLOGY_CONFIG
from generate_stack_topology import normalize_topology
from generate_stack_topology import resolve_repo_path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT_MANIFEST = REPO_ROOT / "build" / "k8s-localdev" / "worlds-localdev.generated.yaml"
DEFAULT_OUTPUT_ACTIVE = REPO_ROOT / "build" / "k8s-localdev" / "topology.active.json"
DEFAULT_OUTPUT_SUMMARY = REPO_ROOT / "build" / "k8s-localdev" / "summary.json"
DEFAULT_NAMESPACE = "dynaxis-localdev"
POSTGRES_RUNTIME_IMAGE = "dynaxis-k8s-postgres:local"
REDIS_RUNTIME_IMAGE = "dynaxis-k8s-redis:local"


def fail(message: str) -> None:
    raise ValueError(message)


def sanitize_kubernetes_name_token(value: str) -> str:
    lowered = [ch.lower() if ch.isalnum() else "-" for ch in value]
    collapsed: list[str] = []
    previous_dash = False
    for ch in lowered:
        if ch == "-":
            if not previous_dash:
                collapsed.append(ch)
            previous_dash = True
            continue
        collapsed.append(ch)
        previous_dash = False

    while collapsed and collapsed[0] == "-":
        collapsed.pop(0)
    while collapsed and collapsed[-1] == "-":
        collapsed.pop()

    return "".join(collapsed) or "pool"


def make_workload_name(world_id: str, shard: str, prefix: str = "world-set") -> str:
    return f"{prefix}-{sanitize_kubernetes_name_token(world_id)}-{sanitize_kubernetes_name_token(shard)}"


def group_server_pools(topology: dict) -> list[dict]:
    grouped: dict[tuple[str, str], dict] = {}
    for server in topology["servers"]:
        key = (server["world_id"], server["shard"])
        pool = grouped.setdefault(
            key,
            {
                "world_id": server["world_id"],
                "shard": server["shard"],
                "replicas": 0,
                "instance_ids": [],
                "extra_tags": [],
            },
        )
        pool["replicas"] += 1
        pool["instance_ids"].append(server["instance_id"])
        for tag in server.get("extra_tags", []):
            if tag not in pool["extra_tags"]:
                pool["extra_tags"].append(tag)

    return sorted(grouped.values(), key=lambda item: (item["world_id"], item["shard"]))


def render_env(name: str, value: str | dict) -> dict:
    if isinstance(value, dict):
        return {"name": name, "valueFrom": value}
    return {"name": name, "value": value}


def server_env(pool: dict, service_name: str) -> list[dict]:
    tags = [f"world:{pool['world_id']}", *pool["extra_tags"]]
    return [
        render_env("POD_NAME", {"fieldRef": {"fieldPath": "metadata.name"}}),
        render_env("POD_NAMESPACE", {"fieldRef": {"fieldPath": "metadata.namespace"}}),
        render_env("PORT", "5000"),
        render_env("DB_URI", "postgresql://dynaxis:password@postgres:5432/dynaxis_db"),
        render_env("REDIS_URI", "tcp://redis:6379"),
        render_env("USE_REDIS_PUBSUB", "1"),
        render_env("REDIS_CHANNEL_PREFIX", "dynaxis:"),
        render_env("WRITE_BEHIND_ENABLED", "1"),
        render_env("REDIS_STREAM_KEY", "session_events"),
        render_env("SERVER_INSTANCE_ID", {"fieldRef": {"fieldPath": "metadata.name"}}),
        render_env("SERVER_ADVERTISE_HOST", f"$(POD_NAME).{service_name}.$(POD_NAMESPACE).svc.cluster.local"),
        render_env("SERVER_ADVERTISE_PORT", "5000"),
        render_env("SERVER_SHARD", pool["shard"]),
        render_env("SERVER_TAGS", ",".join(tags)),
        render_env("SERVER_REGISTRY_PREFIX", "gateway/instances/"),
        render_env("SERVER_REGISTRY_TTL", "30"),
        render_env("SERVER_HEARTBEAT_INTERVAL", "5"),
        render_env("GATEWAY_ID", "$(POD_NAME)"),
        render_env("METRICS_PORT", "9090"),
        render_env("RECENT_HISTORY_LIMIT", "20"),
        render_env("SESSION_CONTINUITY_ENABLED", "1"),
        render_env("SESSION_CONTINUITY_LEASE_TTL_SEC", "900"),
        render_env("ADMIN_COMMAND_SIGNING_SECRET", "dev-admin-command-secret"),
        render_env("ADMIN_COMMAND_TTL_MS", "60000"),
        render_env("ADMIN_COMMAND_FUTURE_SKEW_MS", "5000"),
        render_env("SERVER_DRAIN_TIMEOUT_MS", "15000"),
        render_env("SERVER_DRAIN_POLL_MS", "100"),
    ]


def deployment_doc(
    *,
    namespace: str,
    name: str,
    image: str,
    command: list[str] | None,
    args: list[str] | None,
    env: list[dict],
    ports: list[dict],
    labels: dict[str, str],
    replicas: int = 1,
    image_pull_policy: str = "IfNotPresent",
    init_containers: list[dict] | None = None,
) -> dict:
    container: dict[str, object] = {
        "name": name,
        "image": image,
        "imagePullPolicy": image_pull_policy,
        "env": env,
        "ports": ports,
    }
    if command:
        container["command"] = command
    if args:
        container["args"] = args

    pod_spec: dict[str, object] = {
        "containers": [container],
    }
    if init_containers:
        pod_spec["initContainers"] = init_containers

    return {
        "apiVersion": "apps/v1",
        "kind": "Deployment",
        "metadata": {"name": name, "namespace": namespace, "labels": labels},
        "spec": {
            "replicas": replicas,
            "selector": {"matchLabels": labels},
            "template": {
                "metadata": {"labels": labels},
                "spec": pod_spec,
            },
        },
    }


def service_doc(
    *,
    namespace: str,
    name: str,
    selector: dict[str, str],
    ports: list[dict],
    headless: bool = False,
) -> dict:
    spec: dict = {
        "selector": selector,
        "ports": ports,
    }
    if headless:
        spec["clusterIP"] = "None"
    return {
        "apiVersion": "v1",
        "kind": "Service",
        "metadata": {"name": name, "namespace": namespace},
        "spec": spec,
    }


def statefulset_doc(
    *,
    namespace: str,
    name: str,
    service_name: str,
    image: str,
    command: list[str] | None,
    args: list[str] | None,
    env: list[dict],
    ports: list[dict],
    labels: dict[str, str],
    replicas: int,
    volume_mounts: list[dict] | None = None,
    volumes: list[dict] | None = None,
    image_pull_policy: str = "IfNotPresent",
    init_containers: list[dict] | None = None,
) -> dict:
    container = {
        "name": name,
        "image": image,
        "imagePullPolicy": image_pull_policy,
        "env": env,
        "ports": ports,
    }
    if command:
        container["command"] = command
    if args:
        container["args"] = args
    if volume_mounts:
        container["volumeMounts"] = volume_mounts

    template_spec: dict[str, object] = {"containers": [container]}
    if volumes:
        template_spec["volumes"] = volumes
    if init_containers:
        template_spec["initContainers"] = init_containers

    return {
        "apiVersion": "apps/v1",
        "kind": "StatefulSet",
        "metadata": {"name": name, "namespace": namespace, "labels": labels},
        "spec": {
            "serviceName": service_name,
            "replicas": replicas,
            "selector": {"matchLabels": labels},
            "template": {
                "metadata": {"labels": labels},
                "spec": template_spec,
            },
        },
    }


def job_doc(
    namespace: str,
    name: str,
    image: str,
    command: list[str] | None,
    args: list[str] | None,
    env: list[dict],
    image_pull_policy: str = "IfNotPresent",
    init_containers: list[dict] | None = None,
) -> dict:
    container = {
        "name": name,
        "image": image,
        "imagePullPolicy": image_pull_policy,
        "env": env,
    }
    if command:
        container["command"] = command
    if args:
        container["args"] = args

    pod_spec: dict[str, object] = {
        "restartPolicy": "OnFailure",
        "containers": [container],
    }
    if init_containers:
        pod_spec["initContainers"] = init_containers

    return {
        "apiVersion": "batch/v1",
        "kind": "Job",
        "metadata": {"name": name, "namespace": namespace},
        "spec": {
            "template": {
                "metadata": {"labels": {"app.kubernetes.io/name": name}},
                "spec": pod_spec,
            }
        },
    }


def postgres_wait_init_container(name: str = "wait-postgres") -> dict:
    return {
        "name": name,
        "image": POSTGRES_RUNTIME_IMAGE,
        "imagePullPolicy": "Never",
        "command": [
            "sh",
            "-c",
            "until pg_isready -h postgres -p 5432 -U dynaxis -d dynaxis_db; do echo waiting for postgres:5432; sleep 1; done",
        ],
    }


def redis_wait_init_container(name: str = "wait-redis") -> dict:
    return {
        "name": name,
        "image": REDIS_RUNTIME_IMAGE,
        "imagePullPolicy": "Never",
        "command": [
            "sh",
            "-c",
            "until redis-cli -h redis -p 6379 ping; do echo waiting for redis:6379; sleep 1; done",
        ],
    }


def build_desired_topology(pools: list[dict], topology_name: str) -> dict:
    return {
        "topology_id": topology_name,
        "revision": 1,
        "updated_at_ms": 0,
        "pools": [
            {
                "world_id": pool["world_id"],
                "shard": pool["shard"],
                "replicas": pool["replicas"],
                "capacity_class": "",
                "placement_tags": pool["extra_tags"],
            }
            for pool in pools
        ],
    }


def build_documents(topology: dict, namespace: str) -> tuple[list[dict], dict]:
    pools = group_server_pools(topology)
    desired_topology = build_desired_topology(pools, topology["name"])

    docs: list[dict] = [
        {"apiVersion": "v1", "kind": "Namespace", "metadata": {"name": namespace}},
        {
            "apiVersion": "v1",
            "kind": "ConfigMap",
            "metadata": {"name": "dynaxis-worlds-topology", "namespace": namespace},
            "data": {
                "topology.active.json": json.dumps(topology, indent=2),
                "desired.topology.json": json.dumps(desired_topology, indent=2),
            },
        },
        service_doc(
            namespace=namespace,
            name="postgres",
            selector={"app.kubernetes.io/name": "postgres"},
            ports=[{"name": "postgres", "port": 5432, "targetPort": 5432}],
        ),
        statefulset_doc(
            namespace=namespace,
            name="postgres",
            service_name="postgres",
            image=POSTGRES_RUNTIME_IMAGE,
            command=["docker-entrypoint.sh", "postgres"],
            args=None,
            env=[
                render_env("POSTGRES_USER", "dynaxis"),
                render_env("POSTGRES_PASSWORD", "password"),
                render_env("POSTGRES_DB", "dynaxis_db"),
            ],
            ports=[{"containerPort": 5432, "name": "postgres"}],
            labels={"app.kubernetes.io/name": "postgres"},
            replicas=1,
            volume_mounts=[{"name": "postgres-data", "mountPath": "/var/lib/postgresql/data"}],
            volumes=[{"name": "postgres-data", "emptyDir": {}}],
            image_pull_policy="Never",
        ),
        service_doc(
            namespace=namespace,
            name="redis",
            selector={"app.kubernetes.io/name": "redis"},
            ports=[{"name": "redis", "port": 6379, "targetPort": 6379}],
        ),
        statefulset_doc(
            namespace=namespace,
            name="redis",
            service_name="redis",
            image=REDIS_RUNTIME_IMAGE,
            command=["redis-server", "--save", "", "--appendonly", "no"],
            args=None,
            env=[],
            ports=[{"containerPort": 6379, "name": "redis"}],
            labels={"app.kubernetes.io/name": "redis"},
            replicas=1,
            image_pull_policy="Never",
        ),
        job_doc(
            namespace,
            "migrator",
            "dynaxis-migrator:local",
            command=None,
            args=["migrate"],
            env=[
                render_env("DB_URI", "postgresql://dynaxis:password@postgres:5432/dynaxis_db"),
                render_env("MIGRATIONS_DIR", "/app/migrations"),
            ],
            image_pull_policy="Never",
            init_containers=[
                postgres_wait_init_container(),
            ],
        ),
        deployment_doc(
            namespace=namespace,
            name="wb-worker",
            image="dynaxis-worker:local",
            command=None,
            args=["worker"],
            env=[
                render_env("DB_URI", "postgresql://dynaxis:password@postgres:5432/dynaxis_db"),
                render_env("REDIS_URI", "tcp://redis:6379"),
                render_env("REDIS_STREAM_KEY", "session_events"),
                render_env("WB_GROUP", "wb_group"),
                render_env("WB_CONSUMER", "worker-1"),
                render_env("METRICS_PORT", "9090"),
            ],
            ports=[{"containerPort": 9090, "name": "metrics"}],
            labels={"app.kubernetes.io/name": "wb-worker"},
            image_pull_policy="Never",
            init_containers=[
                postgres_wait_init_container(),
                redis_wait_init_container(),
            ],
        ),
        service_doc(
            namespace=namespace,
            name="wb-worker",
            selector={"app.kubernetes.io/name": "wb-worker"},
            ports=[{"name": "metrics", "port": 9090, "targetPort": 9090}],
        ),
        deployment_doc(
            namespace=namespace,
            name="admin-app",
            image="dynaxis-admin:local",
            command=None,
            args=["admin"],
            env=[
                render_env("REDIS_URI", "tcp://redis:6379"),
                render_env("REDIS_CHANNEL_PREFIX", "dynaxis:"),
                render_env("SERVER_REGISTRY_PREFIX", "gateway/instances/"),
                render_env("SERVER_REGISTRY_TTL", "30"),
                render_env("GATEWAY_SESSION_PREFIX", "gateway/session/"),
                render_env("WB_WORKER_METRICS_URL", "http://wb-worker:9090/metrics"),
                render_env("METRICS_PORT", "39200"),
                render_env("ADMIN_POLL_INTERVAL_MS", "1000"),
                render_env("ADMIN_COMMAND_SIGNING_SECRET", "dev-admin-command-secret"),
            ],
            ports=[{"containerPort": 39200, "name": "http"}],
            labels={"app.kubernetes.io/name": "admin-app"},
            image_pull_policy="Never",
            init_containers=[
                redis_wait_init_container(),
            ],
        ),
        service_doc(
            namespace=namespace,
            name="admin-app",
            selector={"app.kubernetes.io/name": "admin-app"},
            ports=[{"name": "http", "port": 39200, "targetPort": 39200}],
        ),
    ]

    for gateway_name, gateway_id in (("gateway-1", "gw1"), ("gateway-2", "gw2")):
        docs.append(
            deployment_doc(
                namespace=namespace,
                name=gateway_name,
                image="dynaxis-gateway:local",
                command=None,
                args=["gateway"],
                env=[
                    render_env("GATEWAY_LISTEN", "0.0.0.0:6000"),
                    render_env("GATEWAY_ID", gateway_id),
                    render_env("REDIS_URI", "tcp://redis:6379"),
                    render_env("REDIS_CHANNEL_PREFIX", "dynaxis:"),
                    render_env("SERVER_REGISTRY_PREFIX", "gateway/instances/"),
                    render_env("SERVER_REGISTRY_TTL", "30"),
                    render_env("ALLOW_ANONYMOUS", "1"),
                    render_env("METRICS_PORT", "6001"),
                ],
                ports=[
                    {"containerPort": 6000, "name": "tcp"},
                    {"containerPort": 6001, "name": "metrics"},
                ],
                labels={"app.kubernetes.io/name": gateway_name},
                image_pull_policy="Never",
                init_containers=[
                    redis_wait_init_container(),
                ],
            )
        )
        docs.append(
            service_doc(
                namespace=namespace,
                name=gateway_name,
                selector={"app.kubernetes.io/name": gateway_name},
                ports=[
                    {"name": "tcp", "port": 6000, "targetPort": 6000},
                    {"name": "metrics", "port": 6001, "targetPort": 6001},
                ],
            )
        )

    server_workloads: list[dict] = []
    resources: list[dict] = []
    for pool in pools:
        workload_name = make_workload_name(pool["world_id"], pool["shard"])
        service_name = f"{workload_name}-headless"
        labels = {
            "app.kubernetes.io/name": workload_name,
            "dynaxis/world-id": pool["world_id"],
            "dynaxis/shard": pool["shard"],
        }
        docs.append(
            service_doc(
                namespace=namespace,
                name=service_name,
                selector=labels,
                ports=[
                    {"name": "server", "port": 5000, "targetPort": 5000},
                    {"name": "metrics", "port": 9090, "targetPort": 9090},
                ],
                headless=True,
            )
        )
        docs.append(
            statefulset_doc(
                namespace=namespace,
                name=workload_name,
                service_name=service_name,
                image="dynaxis-server:local",
                command=None,
                args=["server"],
                env=server_env(pool, service_name),
                ports=[
                    {"containerPort": 5000, "name": "server"},
                    {"containerPort": 9090, "name": "metrics"},
                ],
                labels=labels,
                replicas=pool["replicas"],
                image_pull_policy="Never",
                init_containers=[
                    postgres_wait_init_container(),
                    redis_wait_init_container(),
                ],
            )
        )
        server_workloads.append(
            {
                "world_id": pool["world_id"],
                "shard": pool["shard"],
                "replicas": pool["replicas"],
                "workload_kind": "StatefulSet",
                "workload_name": workload_name,
                "service_name": service_name,
                "instance_ids": pool["instance_ids"],
                "extra_tags": pool["extra_tags"],
            }
        )

    for doc in docs:
        resources.append({"kind": doc["kind"], "name": doc["metadata"]["name"]})

    summary = {
        "name": topology["name"],
        "source_config": topology["source_config"],
        "namespace": namespace,
        "servers": topology["servers"],
        "desired_topology": desired_topology,
        "server_workloads": server_workloads,
        "resources": resources,
    }
    return docs, summary


def yaml_scalar(value: object) -> str:
    if value is True:
        return "true"
    if value is False:
        return "false"
    if value is None:
        return "null"
    if isinstance(value, (int, float)):
        return str(value)
    return json.dumps(str(value))


def render_yaml(value: object, indent: int = 0, *, list_item: bool = False) -> list[str]:
    prefix = " " * indent
    if isinstance(value, dict):
        if not value:
            return [f"{prefix}{{}}"]
        lines: list[str] = []
        for key, item in value.items():
            if isinstance(item, str) and "\n" in item:
                lines.append(f"{prefix}{key}: |-")
                for line in item.splitlines():
                    lines.append(f"{prefix}  {line}")
            elif isinstance(item, (dict, list)):
                if isinstance(item, list) and not item:
                    lines.append(f"{prefix}{key}: []")
                elif isinstance(item, dict) and not item:
                    lines.append(f"{prefix}{key}: {{}}")
                else:
                    lines.append(f"{prefix}{key}:")
                    lines.extend(render_yaml(item, indent + 2))
            else:
                lines.append(f"{prefix}{key}: {yaml_scalar(item)}")
        return lines

    if isinstance(value, list):
        lines = []
        for item in value:
            if isinstance(item, (dict, list)):
                lines.append(f"{prefix}-")
                lines.extend(render_yaml(item, indent + 2))
            elif isinstance(item, str) and "\n" in item:
                lines.append(f"{prefix}- |-")
                for line in item.splitlines():
                    lines.append(f"{prefix}    {line}")
            else:
                lines.append(f"{prefix}- {yaml_scalar(item)}")
        return lines

    return [f"{prefix}{yaml_scalar(value)}"]


def render_manifest(documents: list[dict]) -> str:
    rendered_docs = []
    for document in documents:
        rendered_docs.append("\n".join(render_yaml(document)))
    return ("\n---\n").join(rendered_docs).rstrip() + "\n"


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a local/dev Kubernetes worlds topology manifest from the shared topology JSON."
    )
    parser.add_argument("--topology-config", default=str(DEFAULT_TOPOLOGY_CONFIG))
    parser.add_argument("--output-manifest", default=str(DEFAULT_OUTPUT_MANIFEST))
    parser.add_argument("--output-active", default=str(DEFAULT_OUTPUT_ACTIVE))
    parser.add_argument("--output-summary", default=str(DEFAULT_OUTPUT_SUMMARY))
    parser.add_argument("--namespace", default=DEFAULT_NAMESPACE)
    args = parser.parse_args()

    try:
        topology_config_path = resolve_repo_path(args.topology_config)
        if not topology_config_path.exists():
            fail(f"topology config not found: {topology_config_path}")

        raw_topology = json.loads(topology_config_path.read_text(encoding="utf-8"))
        topology = normalize_topology(raw_topology, topology_config_path)
        documents, summary = build_documents(topology, args.namespace)

        write_text(resolve_repo_path(args.output_manifest), render_manifest(documents))
        write_json(resolve_repo_path(args.output_active), topology)
        write_json(resolve_repo_path(args.output_summary), summary)

        print(f"generated manifest: {resolve_repo_path(args.output_manifest)}")
        print(f"generated topology snapshot: {resolve_repo_path(args.output_active)}")
        print(f"generated summary: {resolve_repo_path(args.output_summary)}")
        return 0
    except Exception as exc:
        print(f"[k8s-topology-generator] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
