#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from generate_stack_topology import DEFAULT_TOPOLOGY_CONFIG
from generate_stack_topology import resolve_repo_path


REPO_ROOT = Path(__file__).resolve().parents[1]
GENERATOR = REPO_ROOT / "scripts" / "generate_k8s_topology.py"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "build" / "k8s-localdev"
DEFAULT_NAMESPACE = "dynaxis-localdev"
DEFAULT_CLUSTER_NAME = "dynaxis-localdev"
REQUIRED_LOCAL_IMAGES = [
    "dynaxis-server:local",
    "dynaxis-gateway:local",
    "dynaxis-worker:local",
    "dynaxis-admin:local",
    "dynaxis-migrator:local",
]
SUPPORT_LOCAL_IMAGES = {
    "dynaxis-k8s-postgres:local": "FROM postgres:16-alpine\n",
    "dynaxis-k8s-redis:local": "FROM redis:7-alpine\n",
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def run(
    command: list[str],
    *,
    cwd: Path = REPO_ROOT,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if check and result.returncode != 0:
        rendered = " ".join(command)
        fail(
            f"command failed ({result.returncode}): {rendered}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def ensure_command(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        fail(f"required command not found: {name}")
    return resolved


def ensure_docker_is_ready(docker: str) -> None:
    run([docker, "info", "--format", "{{.ServerVersion}}"])


def ensure_local_images(docker: str) -> None:
    missing: list[str] = []
    for image in REQUIRED_LOCAL_IMAGES:
        result = run([docker, "image", "inspect", image], check=False)
        if result.returncode != 0:
            missing.append(image)

    if missing:
        guidance = "pwsh scripts/deploy_docker.ps1 -Action build"
        fail(
            "required local Docker images are missing: "
            + ", ".join(missing)
            + f"\nBuild them first, for example with: {guidance}"
        )


def ensure_support_images(docker: str) -> None:
    for image, dockerfile in SUPPORT_LOCAL_IMAGES.items():
        listed = run([docker, "images", "-q", image], check=False)
        if listed.returncode == 0 and listed.stdout.strip():
            continue

        result = subprocess.run(
            [docker, "build", "-t", image, "-f", "-", "."],
            cwd=REPO_ROOT,
            input=dockerfile,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if result.returncode != 0:
            fail(
                f"failed to build local support image {image}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )


def generate_manifest(topology_config: str, output_dir: Path, namespace: str) -> tuple[Path, Path]:
    manifest_path = output_dir / "worlds-localdev.generated.yaml"
    active_path = output_dir / "topology.active.json"
    summary_path = output_dir / "summary.json"
    command = [
        sys.executable,
        str(GENERATOR),
        "--topology-config",
        topology_config,
        "--output-manifest",
        str(manifest_path),
        "--output-active",
        str(active_path),
        "--output-summary",
        str(summary_path),
        "--namespace",
        namespace,
    ]
    run(command)
    return manifest_path, summary_path


def kind_context_name(cluster_name: str) -> str:
    return f"kind-{cluster_name}"


def kind_cluster_exists(kind: str, cluster_name: str) -> bool:
    result = run([kind, "get", "clusters"], check=False)
    if result.returncode != 0:
        return False
    clusters = {line.strip() for line in result.stdout.splitlines() if line.strip()}
    return cluster_name in clusters


def ensure_cluster(kind: str, cluster_name: str, wait_seconds: int, recreate: bool) -> None:
    if recreate and kind_cluster_exists(kind, cluster_name):
        run([kind, "delete", "cluster", "--name", cluster_name])

    if not kind_cluster_exists(kind, cluster_name):
        run(
            [
                kind,
                "create",
                "cluster",
                "--name",
                cluster_name,
                "--wait",
                f"{wait_seconds}s",
            ]
        )


def load_images_into_cluster(kind: str, cluster_name: str) -> None:
    command = [
        kind,
        "load",
        "docker-image",
        *REQUIRED_LOCAL_IMAGES,
        *SUPPORT_LOCAL_IMAGES.keys(),
        "--name",
        cluster_name,
    ]
    run(command)


def kubectl_base(kubectl: str, cluster_name: str) -> list[str]:
    return [kubectl, "--context", kind_context_name(cluster_name)]


def kubectl_delete_namespace(
    kubectl: str,
    cluster_name: str,
    namespace: str,
    wait_seconds: int,
) -> None:
    delete_result = run(
        [
            *kubectl_base(kubectl, cluster_name),
            "delete",
            "namespace",
            namespace,
            "--ignore-not-found=true",
            "--wait=true",
            f"--timeout={wait_seconds}s",
        ],
        check=False,
    )
    if delete_result.returncode not in (0,):
        fail(
            f"failed to delete namespace {namespace}\nstdout:\n{delete_result.stdout}\nstderr:\n{delete_result.stderr}"
        )


def kubectl_apply_manifest(kubectl: str, cluster_name: str, manifest_path: Path) -> None:
    run([*kubectl_base(kubectl, cluster_name), "apply", "-f", str(manifest_path)])


def wait_for_job(kubectl: str, cluster_name: str, namespace: str, name: str, wait_seconds: int) -> None:
    command = [
        *kubectl_base(kubectl, cluster_name),
        "--namespace",
        namespace,
        "wait",
        "--for=condition=complete",
        f"job/{name}",
        f"--timeout={wait_seconds}s",
    ]
    result = run(command, check=False)
    if result.returncode == 0:
        return

    diagnostics = collect_namespace_diagnostics(kubectl, cluster_name, namespace)
    fail(
        f"job readiness failed for {name}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}\n"
        f"namespace diagnostics:\n{diagnostics}"
    )


def wait_for_rollout(
    kubectl: str,
    cluster_name: str,
    namespace: str,
    kind_name: str,
    resource_name: str,
    wait_seconds: int,
) -> None:
    command = [
        *kubectl_base(kubectl, cluster_name),
        "--namespace",
        namespace,
        "rollout",
        "status",
        f"{kind_name}/{resource_name}",
        f"--timeout={wait_seconds}s",
    ]
    result = run(command, check=False)
    if result.returncode == 0:
        return

    diagnostics = collect_namespace_diagnostics(kubectl, cluster_name, namespace)
    fail(
        f"rollout status failed for {kind_name}/{resource_name}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}\n"
        f"namespace diagnostics:\n{diagnostics}"
    )


def collect_namespace_diagnostics(kubectl: str, cluster_name: str, namespace: str) -> str:
    outputs: list[str] = []
    for label, command in (
        (
            "workloads",
            [
                *kubectl_base(kubectl, cluster_name),
                "--namespace",
                namespace,
                "get",
                "pods,deployments,statefulsets,jobs,services",
                "-o",
                "wide",
            ],
        ),
        (
            "events",
            [
                *kubectl_base(kubectl, cluster_name),
                "--namespace",
                namespace,
                "get",
                "events",
                "--sort-by=.lastTimestamp",
            ],
        ),
    ):
        result = run(command, check=False)
        outputs.append(f"[{label}]\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return "\n".join(outputs)


def capture_cluster_state(
    kubectl: str,
    cluster_name: str,
    namespace: str,
    manifest_path: Path,
    summary_path: Path,
    output_dir: Path,
) -> Path:
    result = run(
        [
            *kubectl_base(kubectl, cluster_name),
            "--namespace",
            namespace,
            "get",
            "deployment,statefulset,job,service,pod,configmap",
            "-o",
            "json",
        ]
    )
    state_path = output_dir / "cluster-state.json"
    payload = {
        "cluster_name": cluster_name,
        "context": kind_context_name(cluster_name),
        "namespace": namespace,
        "manifest_path": str(manifest_path),
        "summary_path": str(summary_path),
        "resources": json.loads(result.stdout),
    }
    write_json(state_path, payload)
    return state_path


def load_summary(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def action_up(args: argparse.Namespace) -> int:
    docker = ensure_command("docker")
    kubectl = ensure_command("kubectl")
    kind = ensure_command("kind")
    ensure_docker_is_ready(docker)
    ensure_local_images(docker)
    ensure_support_images(docker)

    output_dir = resolve_repo_path(str(args.output_dir))
    manifest_path, summary_path = generate_manifest(args.topology_config, output_dir, args.namespace)
    summary = load_summary(summary_path)

    ensure_cluster(kind, args.cluster_name, args.wait_timeout_seconds, args.recreate_cluster)
    if args.clean_namespace:
        kubectl_delete_namespace(
            kubectl,
            args.cluster_name,
            args.namespace,
            args.wait_timeout_seconds,
        )

    if not args.skip_image_load:
        load_images_into_cluster(kind, args.cluster_name)

    kubectl_apply_manifest(kubectl, args.cluster_name, manifest_path)
    wait_for_rollout(kubectl, args.cluster_name, args.namespace, "statefulset", "postgres", args.wait_timeout_seconds)
    wait_for_rollout(kubectl, args.cluster_name, args.namespace, "statefulset", "redis", args.wait_timeout_seconds)
    wait_for_job(kubectl, args.cluster_name, args.namespace, "migrator", args.wait_timeout_seconds)

    for deployment_name in ("wb-worker", "admin-app", "gateway-1", "gateway-2"):
        wait_for_rollout(
            kubectl,
            args.cluster_name,
            args.namespace,
            "deployment",
            deployment_name,
            args.wait_timeout_seconds,
        )

    for workload in summary.get("server_workloads", []):
        workload_name = workload.get("workload_name")
        if isinstance(workload_name, str) and workload_name:
            wait_for_rollout(
                kubectl,
                args.cluster_name,
                args.namespace,
                "statefulset",
                workload_name,
                args.wait_timeout_seconds,
            )

    state_path = capture_cluster_state(
        kubectl,
        args.cluster_name,
        args.namespace,
        manifest_path,
        summary_path,
        output_dir,
    )
    print(f"cluster context: {kind_context_name(args.cluster_name)}")
    print(f"manifest: {manifest_path}")
    print(f"summary: {summary_path}")
    print(f"cluster state: {state_path}")
    return 0


def action_status(args: argparse.Namespace) -> int:
    kubectl = ensure_command("kubectl")
    kind = ensure_command("kind")
    if not kind_cluster_exists(kind, args.cluster_name):
        fail(f"kind cluster does not exist: {args.cluster_name}")

    output_dir = resolve_repo_path(str(args.output_dir))
    manifest_path = output_dir / "worlds-localdev.generated.yaml"
    summary_path = output_dir / "summary.json"
    state_path = capture_cluster_state(
        kubectl,
        args.cluster_name,
        args.namespace,
        manifest_path,
        summary_path,
        output_dir,
    )
    print(f"cluster state: {state_path}")
    return 0


def action_down(args: argparse.Namespace) -> int:
    kind = ensure_command("kind")
    if kind_cluster_exists(kind, args.cluster_name):
        run([kind, "delete", "cluster", "--name", args.cluster_name])
        print(f"deleted cluster: {args.cluster_name}")
    else:
        print(f"cluster already absent: {args.cluster_name}")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the topology-driven local/dev Kubernetes worlds stack on a kind cluster."
    )
    parser.add_argument("action", choices=("up", "status", "down"))
    parser.add_argument("--cluster-name", default=DEFAULT_CLUSTER_NAME)
    parser.add_argument("--topology-config", default=str(DEFAULT_TOPOLOGY_CONFIG))
    parser.add_argument("--namespace", default=DEFAULT_NAMESPACE)
    parser.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--clean-namespace", action="store_true")
    parser.add_argument("--recreate-cluster", action="store_true")
    parser.add_argument("--skip-image-load", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        args.topology_config = str(resolve_repo_path(str(args.topology_config)))
        if args.action == "up":
            return action_up(args)
        if args.action == "status":
            return action_status(args)
        return action_down(args)
    except Exception as exc:
        print(f"[k8s-localdev-kind] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
