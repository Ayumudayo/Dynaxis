from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

from verify_worlds_kubernetes_kind import DEFAULT_TOPOLOGY
from verify_worlds_kubernetes_kind import RUNNER
from verify_worlds_kubernetes_kind import detect_prerequisites
from verify_worlds_kubernetes_kind import run
from verify_worlds_kubernetes_kind import skip
from verify_worlds_kubernetes_kind_control_plane import (
    ADMIN_SERVICE_PORT,
    PortForward,
    load_json_http,
    request_json_http,
    wait_for_observed_topology,
    wait_for_port_forward,
)
from verify_worlds_kubernetes_kind_metrics_budget import fetch_metrics_text, parse_metric_value


REPO_ROOT = Path(__file__).resolve().parents[2]


def write_outage_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "redis-outage.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"redis outage report: {report_path}")


def read_metric(base_url: str, metric_name: str, labels: dict[str, str] | None = None) -> float:
    return parse_metric_value(fetch_metrics_text(base_url), metric_name, labels)


def http_text(base_url: str, path: str) -> tuple[int, str]:
    request = urllib.request.Request(f"{base_url}{path}", method="GET")
    try:
        with urllib.request.urlopen(request, timeout=5.0) as response:
            return response.status, response.read().decode("utf-8", errors="replace").strip()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", errors="replace").strip()


def wait_for_metric_value(
    base_url: str,
    metric_name: str,
    predicate,
    *,
    labels: dict[str, str] | None = None,
    timeout_seconds: float,
) -> float:
    deadline = time.time() + timeout_seconds
    last_value = 0.0
    while time.time() < deadline:
        last_value = read_metric(base_url, metric_name, labels)
        if predicate(last_value):
            return last_value
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for metric {metric_name}: last_value={last_value}")


def wait_for_instances_status(base_url: str, expected_status: int, timeout_seconds: float) -> tuple[int, dict | None]:
    deadline = time.time() + timeout_seconds
    last_status = 0
    last_payload: dict | None = None
    while time.time() < deadline:
        last_status, last_payload = request_json_http(base_url, "/api/v1/instances?limit=1")
        if last_status == expected_status:
            return last_status, last_payload
        time.sleep(0.5)
    raise RuntimeError(
        f"timeout waiting for /api/v1/instances status={expected_status}: last_status={last_status} payload={last_payload}"
    )


def wait_for_recovered_observed_topology(
    base_url: str,
    *,
    minimum_worlds: int,
    minimum_instances: int,
    timeout_seconds: float,
) -> dict:
    deadline = time.time() + timeout_seconds
    last_payload: dict | None = None
    while time.time() < deadline:
        payload = load_json_http(base_url, "/api/v1/topology/observed?timeout_ms=5000")
        last_payload = payload
        summary = payload.get("data", {}).get("summary", {})
        worlds_total = int(summary.get("worlds_total") or 0)
        instances_total = int(summary.get("instances_total") or 0)
        if worlds_total >= minimum_worlds and instances_total >= minimum_instances:
            return payload
        time.sleep(0.5)
    raise RuntimeError(f"observed topology did not recover expected shape: {last_payload}")


def scale_redis(cluster_name: str, namespace: str, replicas: int) -> None:
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "scale",
            "statefulset/redis",
            f"--replicas={replicas}",
        ]
    )


def wait_for_redis_pod_absent(cluster_name: str, namespace: str, timeout_seconds: float) -> None:
    deadline = time.time() + timeout_seconds
    last_items = 0
    while time.time() < deadline:
        result = run(
            [
                "kubectl",
                "--context",
                f"kind-{cluster_name}",
                "--namespace",
                namespace,
                "get",
                "pods",
                "-l",
                "app.kubernetes.io/name=redis",
                "-o",
                "json",
            ]
        )
        payload = json.loads(result.stdout or "{}")
        last_items = len(payload.get("items", []))
        if last_items == 0:
            return
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for redis pod removal: remaining={last_items}")


def wait_for_redis_pod_ready(cluster_name: str, namespace: str, timeout_seconds: int) -> None:
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "wait",
            "--for=condition=Ready",
            "pod/redis-0",
            f"--timeout={timeout_seconds}s",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a live kind Redis dependency outage/recovery proof for admin_app."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    cluster_name = f"dynaxis-redis-outage-{int(time.time())}"
    namespace = "dynaxis-redis-outage"

    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-redis-outage-") as temp_dir_raw:
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
            with PortForward(cluster_name, namespace, "admin-app", ADMIN_SERVICE_PORT) as admin_forward:
                base_url = wait_for_port_forward(admin_forward, timeout_seconds=30.0)
                status, body = http_text(base_url, "/readyz")
                if status != 200 or body != "ready":
                    raise RuntimeError(f"admin readyz mismatch before outage: status={status} body={body!r}")

                observed_before = wait_for_observed_topology(base_url, timeout_seconds=30.0)
                instances_before = load_json_http(base_url, "/api/v1/instances?limit=100")
                before_summary = observed_before.get("data", {}).get("summary", {})
                before_total_instances = len(instances_before.get("data", {}).get("items", []))
                if before_total_instances == 0:
                    raise RuntimeError("instances endpoint returned no items before outage")

                before_admin_redis = read_metric(base_url, "admin_redis_available")
                before_dep_redis = read_metric(
                    base_url,
                    "runtime_dependency_ready",
                    {"name": "redis", "required": "false"},
                )
                before_deps_ok = read_metric(base_url, "runtime_dependencies_ok")
                if before_admin_redis != 1.0 or before_dep_redis != 1.0 or before_deps_ok != 1.0:
                    raise RuntimeError(
                        "admin dependency metrics were not healthy before outage: "
                        f"admin_redis={before_admin_redis} dep_redis={before_dep_redis} deps_ok={before_deps_ok}"
                    )

                scale_redis(cluster_name, namespace, 0)
                wait_for_redis_pod_absent(cluster_name, namespace, timeout_seconds=60.0)

                outage_admin_redis = wait_for_metric_value(
                    base_url,
                    "admin_redis_available",
                    lambda value: value == 0.0,
                    timeout_seconds=30.0,
                )
                outage_dep_redis = wait_for_metric_value(
                    base_url,
                    "runtime_dependency_ready",
                    lambda value: value == 0.0,
                    labels={"name": "redis", "required": "false"},
                    timeout_seconds=30.0,
                )
                outage_deps_ok = read_metric(base_url, "runtime_dependencies_ok")

                status, body = http_text(base_url, "/readyz")
                if status != 200 or body != "ready":
                    raise RuntimeError(f"admin readyz mismatch during outage: status={status} body={body!r}")

                instances_outage_status, instances_outage_payload = wait_for_instances_status(
                    base_url,
                    503,
                    timeout_seconds=30.0,
                )

                scale_redis(cluster_name, namespace, 1)
                wait_for_redis_pod_ready(cluster_name, namespace, timeout_seconds=args.wait_timeout_seconds)

                recovered_admin_redis = wait_for_metric_value(
                    base_url,
                    "admin_redis_available",
                    lambda value: value == 1.0,
                    timeout_seconds=60.0,
                )
                recovered_dep_redis = wait_for_metric_value(
                    base_url,
                    "runtime_dependency_ready",
                    lambda value: value == 1.0,
                    labels={"name": "redis", "required": "false"},
                    timeout_seconds=60.0,
                )
                recovered_deps_ok = read_metric(base_url, "runtime_dependencies_ok")

                status, body = http_text(base_url, "/readyz")
                if status != 200 or body != "ready":
                    raise RuntimeError(f"admin readyz mismatch after outage: status={status} body={body!r}")

                observed_after = wait_for_recovered_observed_topology(
                    base_url,
                    minimum_worlds=int(before_summary.get("worlds_total") or 0),
                    minimum_instances=before_total_instances,
                    timeout_seconds=float(args.wait_timeout_seconds),
                )
                instances_after = load_json_http(base_url, "/api/v1/instances?limit=100")
                after_summary = observed_after.get("data", {}).get("summary", {})
                after_total_instances = len(instances_after.get("data", {}).get("items", []))
                if int(after_summary.get("worlds_total") or 0) < 2:
                    raise RuntimeError(f"observed topology did not recover expected world count: {after_summary}")
                if after_total_instances == 0:
                    raise RuntimeError("instances endpoint returned no items after outage recovery")

                report = {
                    "proof": "worlds-kubernetes-kind-redis-outage",
                    "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                    "before": {
                        "worlds_total": int(before_summary.get("worlds_total") or 0),
                        "instances_total": before_total_instances,
                        "admin_redis_available": before_admin_redis,
                        "runtime_dependency_ready_redis": before_dep_redis,
                        "runtime_dependencies_ok": before_deps_ok,
                    },
                    "outage": {
                        "admin_redis_available": outage_admin_redis,
                        "runtime_dependency_ready_redis": outage_dep_redis,
                        "runtime_dependencies_ok": outage_deps_ok,
                        "instances_status": instances_outage_status,
                        "instances_payload": instances_outage_payload,
                    },
                    "recovered": {
                        "worlds_total": int(after_summary.get("worlds_total") or 0),
                        "instances_total": after_total_instances,
                        "admin_redis_available": recovered_admin_redis,
                        "runtime_dependency_ready_redis": recovered_dep_redis,
                        "runtime_dependencies_ok": recovered_deps_ok,
                    },
                }
                write_outage_report(report, args.artifact_dir)
                print(
                    "PASS worlds-kubernetes-kind-redis-outage: "
                    f"instances_before={before_total_instances} instances_after={after_total_instances} "
                    f"outage_status={instances_outage_status}"
                )
                return 0
        except Exception as exc:
            print(f"FAIL worlds-kubernetes-kind-redis-outage: {exc}")
            return 1
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="")


if __name__ == "__main__":
    raise SystemExit(main())
