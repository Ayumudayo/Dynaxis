from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "scripts" / "run_k8s_localdev_kind.py"
DEFAULT_TOPOLOGY = REPO_ROOT / "docker" / "stack" / "topologies" / "default.json"
REQUIRED_LOCAL_IMAGES = [
    "dynaxis-server:local",
    "dynaxis-gateway:local",
    "dynaxis-worker:local",
    "dynaxis-admin:local",
    "dynaxis-migrator:local",
]


def run(command: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if check and result.returncode != 0:
        rendered = " ".join(command)
        raise RuntimeError(
            f"command failed ({result.returncode}): {rendered}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def skip(message: str) -> int:
    print(f"SKIP worlds-kubernetes-kind: {message}")
    return 77


def detect_prerequisites() -> str | None:
    for command_name in ("docker", "kubectl", "kind"):
        if shutil.which(command_name) is None:
            return f"{command_name} is not installed"

    docker_info = run(["docker", "info", "--format", "{{.ServerVersion}}"], check=False)
    if docker_info.returncode != 0:
        return "docker daemon is not reachable"

    for image in REQUIRED_LOCAL_IMAGES:
        image_check = run(["docker", "image", "inspect", image], check=False)
        if image_check.returncode != 0:
            return f"required local Docker image is missing: {image}"

    return None


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def find_resource(resources: dict, kind: str, name: str) -> dict:
    for item in resources.get("items", []):
        if item.get("kind") == kind and item.get("metadata", {}).get("name") == name:
            return item
    raise RuntimeError(f"resource not found: {kind}/{name}")


def assert_complete_job(job: dict) -> None:
    conditions = job.get("status", {}).get("conditions", [])
    for condition in conditions:
        if condition.get("type") == "Complete" and condition.get("status") == "True":
            return
    raise RuntimeError("migrator job did not report Complete=True")


def assert_statefulset_ready(statefulset: dict) -> None:
    replicas = int(statefulset.get("spec", {}).get("replicas", 0))
    ready_replicas = int(statefulset.get("status", {}).get("readyReplicas", 0))
    if ready_replicas != replicas:
        raise RuntimeError(
            f"statefulset not fully ready: {statefulset.get('metadata', {}).get('name')} "
            f"ready={ready_replicas} replicas={replicas}"
        )


def assert_deployment_ready(deployment: dict) -> None:
    replicas = int(deployment.get("spec", {}).get("replicas", 0))
    ready_replicas = int(deployment.get("status", {}).get("readyReplicas", 0))
    if ready_replicas != replicas:
        raise RuntimeError(
            f"deployment not fully ready: {deployment.get('metadata', {}).get('name')} "
            f"ready={ready_replicas} replicas={replicas}"
        )


def assert_pods_healthy(resources: dict) -> None:
    allowed_phases = {"Running", "Succeeded"}
    for item in resources.get("items", []):
        if item.get("kind") != "Pod":
            continue
        phase = item.get("status", {}).get("phase")
        name = item.get("metadata", {}).get("name")
        if phase not in allowed_phases:
            raise RuntimeError(f"pod is not healthy: {name} phase={phase}")


def verify_cluster_state(state_path: Path, summary_path: Path) -> None:
    cluster_state = load_json(state_path)
    resources = cluster_state.get("resources", {})
    summary = load_json(summary_path)

    find_resource(resources, "ConfigMap", "dynaxis-worlds-topology")
    assert_complete_job(find_resource(resources, "Job", "migrator"))

    for service_name in ("postgres", "redis", "wb-worker", "admin-app", "gateway-1", "gateway-2"):
        find_resource(resources, "Service", service_name)

    assert_statefulset_ready(find_resource(resources, "StatefulSet", "postgres"))
    assert_statefulset_ready(find_resource(resources, "StatefulSet", "redis"))

    for deployment_name in ("wb-worker", "admin-app", "gateway-1", "gateway-2"):
        assert_deployment_ready(find_resource(resources, "Deployment", deployment_name))

    for workload in summary.get("server_workloads", []):
        workload_name = workload.get("workload_name")
        if not isinstance(workload_name, str) or not workload_name:
            raise RuntimeError(f"invalid workload name in summary: {workload}")
        assert_statefulset_ready(find_resource(resources, "StatefulSet", workload_name))
        find_resource(resources, "Service", workload.get("service_name", ""))

    assert_pods_healthy(resources)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run an optional live kind proof for the local/dev Kubernetes worlds harness."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    cluster_name = f"dynaxis-proof-{int(time.time())}"
    namespace = "dynaxis-proof"

    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-proof-") as temp_dir_raw:
        output_dir = Path(temp_dir_raw)
        up_command = [
            sys.executable,
            str(RUNNER),
            "up",
            "--cluster-name",
            cluster_name,
            "--topology-config",
            str(DEFAULT_TOPOLOGY),
            "--namespace",
            namespace,
            "--output-dir",
            str(output_dir),
            "--wait-timeout-seconds",
            str(args.wait_timeout_seconds),
            "--clean-namespace",
            "--recreate-cluster",
        ]
        down_command = [
            sys.executable,
            str(RUNNER),
            "down",
            "--cluster-name",
            cluster_name,
        ]

        try:
            run(up_command)
            verify_cluster_state(output_dir / "cluster-state.json", output_dir / "summary.json")
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="", file=sys.stderr)

    print("PASS worlds-kubernetes-kind")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
