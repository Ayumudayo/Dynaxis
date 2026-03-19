from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
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
    wait_for_port_forward,
)
from verify_worlds_kubernetes_kind_redis_outage import http_text, read_metric, wait_for_metric_value


REPO_ROOT = Path(__file__).resolve().parents[2]


def write_worker_outage_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "worker-outage.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"worker outage report: {report_path}")


def delete_worker_service(cluster_name: str, namespace: str) -> None:
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "delete",
            "service/wb-worker",
        ]
    )


def restore_worker_service(cluster_name: str, namespace: str, manifest_path: Path) -> None:
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "apply",
            "-f",
            str(manifest_path),
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a live kind wb_worker metrics outage/recovery proof for admin_app."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    cluster_name = f"dynaxis-worker-outage-{int(time.time())}"
    namespace = "dynaxis-worker-outage"

    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-worker-outage-") as temp_dir_raw:
        output_dir = Path(temp_dir_raw)
        manifest_path = output_dir / "worlds-localdev.generated.yaml"
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

                worker_before = load_json_http(base_url, "/api/v1/worker/write-behind")
                before_available = read_metric(base_url, "admin_worker_metrics_available")
                before_dep = read_metric(
                    base_url,
                    "runtime_dependency_ready",
                    {"name": "wb_metrics", "required": "false"},
                )
                before_deps_ok = read_metric(base_url, "runtime_dependencies_ok")
                if before_available != 1.0 or before_dep != 1.0 or before_deps_ok != 1.0:
                    raise RuntimeError(
                        "worker dependency metrics were not healthy before outage: "
                        f"available={before_available} dep={before_dep} deps_ok={before_deps_ok}"
                    )

                delete_worker_service(cluster_name, namespace)

                outage_available = wait_for_metric_value(
                    base_url,
                    "admin_worker_metrics_available",
                    lambda value: value == 0.0,
                    timeout_seconds=30.0,
                )
                outage_dep = wait_for_metric_value(
                    base_url,
                    "runtime_dependency_ready",
                    lambda value: value == 0.0,
                    labels={"name": "wb_metrics", "required": "false"},
                    timeout_seconds=30.0,
                )
                outage_deps_ok = read_metric(base_url, "runtime_dependencies_ok")
                outage_status, outage_payload = request_json_http(base_url, "/api/v1/worker/write-behind")
                if outage_status != 503:
                    raise RuntimeError(f"worker endpoint did not degrade to 503: status={outage_status} payload={outage_payload}")

                status, body = http_text(base_url, "/readyz")
                if status != 200 or body != "ready":
                    raise RuntimeError(f"admin readyz mismatch during outage: status={status} body={body!r}")

                restore_worker_service(cluster_name, namespace, manifest_path)

                recovered_available = wait_for_metric_value(
                    base_url,
                    "admin_worker_metrics_available",
                    lambda value: value == 1.0,
                    timeout_seconds=60.0,
                )
                recovered_dep = wait_for_metric_value(
                    base_url,
                    "runtime_dependency_ready",
                    lambda value: value == 1.0,
                    labels={"name": "wb_metrics", "required": "false"},
                    timeout_seconds=60.0,
                )
                recovered_deps_ok = read_metric(base_url, "runtime_dependencies_ok")
                worker_after = load_json_http(base_url, "/api/v1/worker/write-behind")

                status, body = http_text(base_url, "/readyz")
                if status != 200 or body != "ready":
                    raise RuntimeError(f"admin readyz mismatch after outage: status={status} body={body!r}")

                report = {
                    "proof": "worlds-kubernetes-kind-worker-outage",
                    "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                    "before": {
                        "admin_worker_metrics_available": before_available,
                        "runtime_dependency_ready_wb_metrics": before_dep,
                        "runtime_dependencies_ok": before_deps_ok,
                        "worker_snapshot": worker_before.get("data", {}),
                    },
                    "outage": {
                        "admin_worker_metrics_available": outage_available,
                        "runtime_dependency_ready_wb_metrics": outage_dep,
                        "runtime_dependencies_ok": outage_deps_ok,
                        "worker_status": outage_status,
                        "worker_payload": outage_payload,
                    },
                    "recovered": {
                        "admin_worker_metrics_available": recovered_available,
                        "runtime_dependency_ready_wb_metrics": recovered_dep,
                        "runtime_dependencies_ok": recovered_deps_ok,
                        "worker_snapshot": worker_after.get("data", {}),
                    },
                }
                write_worker_outage_report(report, args.artifact_dir)
                print(
                    "PASS worlds-kubernetes-kind-worker-outage: "
                    f"outage_status={outage_status} recovered_pending={worker_after.get('data', {}).get('pending')}"
                )
                return 0
        except Exception as exc:
            print(f"FAIL worlds-kubernetes-kind-worker-outage: {exc}")
            return 1
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="")


if __name__ == "__main__":
    raise SystemExit(main())
