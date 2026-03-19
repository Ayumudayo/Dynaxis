from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "scripts" / "generate_k8s_topology.py"
DEFAULT_TOPOLOGY = REPO_ROOT / "docker" / "stack" / "topologies" / "default.json"
SAME_WORLD_TOPOLOGY = REPO_ROOT / "docker" / "stack" / "topologies" / "mmorpg-same-world-proof.json"


def run_generator(topology_config: Path, output_dir: Path, namespace: str) -> tuple[Path, Path]:
    manifest_path = output_dir / "worlds-localdev.generated.yaml"
    summary_path = output_dir / "summary.json"
    active_path = output_dir / "topology.active.json"
    command = [
        sys.executable,
        str(GENERATOR),
        "--topology-config",
        str(topology_config),
        "--output-manifest",
        str(manifest_path),
        "--output-active",
        str(active_path),
        "--output-summary",
        str(summary_path),
        "--namespace",
        namespace,
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
            f"generator failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return manifest_path, summary_path


def load_summary(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def assert_default_summary(summary: dict) -> None:
    if summary.get("namespace") != "dynaxis-localdev":
        raise RuntimeError("default summary namespace mismatch")

    pools = summary.get("server_workloads")
    if not isinstance(pools, list) or len(pools) != 2:
        raise RuntimeError("default summary expected exactly two server workloads")

    pool_names = {(pool["world_id"], pool["shard"]): pool for pool in pools}
    expected = {
        ("starter-a", "stack-shard-a"): ("world-set-starter-a-stack-shard-a", 1),
        ("starter-b", "stack-shard-b"): ("world-set-starter-b-stack-shard-b", 1),
    }
    for key, (workload_name, replicas) in expected.items():
        pool = pool_names.get(key)
        if pool is None:
            raise RuntimeError(f"missing default pool {key}")
        if pool.get("workload_name") != workload_name:
            raise RuntimeError(f"default workload name mismatch for {key}: {pool}")
        if pool.get("replicas") != replicas:
            raise RuntimeError(f"default replica mismatch for {key}: {pool}")

    resources = {(item["kind"], item["name"]) for item in summary.get("resources", [])}
    required_resources = {
        ("Namespace", "dynaxis-localdev"),
        ("Service", "postgres"),
        ("StatefulSet", "postgres"),
        ("Service", "redis"),
        ("StatefulSet", "redis"),
        ("Deployment", "gateway-1"),
        ("Deployment", "gateway-2"),
        ("Deployment", "admin-app"),
        ("Deployment", "wb-worker"),
        ("Job", "migrator"),
    }
    missing = required_resources - resources
    if missing:
        raise RuntimeError(f"default summary missing resources: {sorted(missing)}")


def assert_same_world_summary(summary: dict) -> None:
    pools = summary.get("server_workloads")
    if not isinstance(pools, list):
        raise RuntimeError("same-world summary missing server workloads")

    same_world = None
    for pool in pools:
        if pool.get("world_id") == "starter-a" and pool.get("shard") == "stack-shard-a":
            same_world = pool
            break
    if same_world is None:
        raise RuntimeError("same-world summary missing starter-a pool")
    if same_world.get("replicas") != 2:
        raise RuntimeError(f"same-world pool expected replicas=2, got {same_world}")

    instance_ids = sorted(same_world.get("instance_ids", []))
    if instance_ids != ["server-1", "server-3"]:
        raise RuntimeError(f"same-world pool instance ids mismatch: {instance_ids}")


def assert_manifest_text(manifest_path: Path) -> None:
    text = manifest_path.read_text(encoding="utf-8")
    for token in (
        'kind: "StatefulSet"',
        'kind: "Deployment"',
        'name: "dynaxis-worlds-topology"',
        'SERVER_INSTANCE_ID',
        'SERVER_ADVERTISE_HOST',
        'SERVER_SHARD',
        'SERVER_TAGS',
    ):
        if token not in text:
            raise RuntimeError(f"generated manifest missing token: {token}")


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def kubectl_validation_prerequisite_reason(kubectl: str) -> str | None:
    current_context = run_command([kubectl, "config", "current-context"])
    if current_context.returncode != 0:
        return "current kubectl context is not set"

    version_result = run_command([kubectl, "version", "--output=json", "--request-timeout=5s"])
    if version_result.returncode != 0:
        return "current kubectl context is not reachable"

    try:
        version_payload = json.loads(version_result.stdout or "{}")
    except json.JSONDecodeError:
        return "kubectl server version probe returned invalid JSON"

    if "serverVersion" not in version_payload:
        return "kubectl server version is unavailable for the current context"

    return None


def maybe_run_kubectl_validation(manifest_path: Path) -> bool:
    kubectl = shutil.which("kubectl")
    if kubectl is None:
        print("SKIP kubectl validation: kubectl is not installed")
        return False

    prerequisite_reason = kubectl_validation_prerequisite_reason(kubectl)
    if prerequisite_reason is not None:
        print(f"SKIP kubectl validation: {prerequisite_reason}")
        return False

    command = [
        kubectl,
        "create",
        "--dry-run=client",
        "--validate=false",
        "-f",
        str(manifest_path),
        "-o",
        "yaml",
    ]
    result = run_command(command)
    if result.returncode != 0:
        raise RuntimeError(
            f"kubectl client validation failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    print(f"PASS kubectl validation: {manifest_path.name}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate the topology-driven local/dev Kubernetes worlds harness."
    )
    parser.add_argument("--kubectl-validate", action="store_true")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="dynaxis-k8s-localdev-") as temp_dir_raw:
        temp_dir = Path(temp_dir_raw)

        default_manifest, default_summary_path = run_generator(
            DEFAULT_TOPOLOGY,
            temp_dir / "default",
            "dynaxis-localdev",
        )
        default_summary = load_summary(default_summary_path)
        assert_default_summary(default_summary)
        assert_manifest_text(default_manifest)

        same_world_manifest, same_world_summary_path = run_generator(
            SAME_WORLD_TOPOLOGY,
            temp_dir / "same-world",
            "dynaxis-localdev",
        )
        same_world_summary = load_summary(same_world_summary_path)
        assert_same_world_summary(same_world_summary)
        assert_manifest_text(same_world_manifest)

        kubectl_validated = False
        if args.kubectl_validate:
            kubectl_validated = maybe_run_kubectl_validation(default_manifest) or kubectl_validated
            kubectl_validated = maybe_run_kubectl_validation(same_world_manifest) or kubectl_validated

    if args.kubectl_validate and not kubectl_validated:
        print("PASS worlds-kubernetes-localdev (kubectl validation skipped)")
        return 0

    print("PASS worlds-kubernetes-localdev")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
