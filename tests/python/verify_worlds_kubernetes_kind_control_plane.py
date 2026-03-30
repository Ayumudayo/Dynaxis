from __future__ import annotations

import argparse
import json
import socket
import subprocess
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


REPO_ROOT = Path(__file__).resolve().parents[2]
ADMIN_SERVICE_PORT = 39200


def choose_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def http_request(base_url: str, path: str, method: str = "GET", body: object | None = None) -> tuple[int, str, bytes]:
    payload = None
    headers: dict[str, str] = {}
    if body is not None:
        payload = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
        headers["Content-Length"] = str(len(payload))

    request = urllib.request.Request(
        f"{base_url}{path}",
        method=method,
        headers=headers,
        data=payload,
    )
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.getheader("Content-Type", ""), response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.headers.get("Content-Type", ""), exc.read()


def load_json_http(base_url: str, path: str, method: str = "GET", body: object | None = None) -> dict:
    status, content_type, payload = http_request(base_url, path, method=method, body=body)
    if status != 200:
        raise RuntimeError(f"{path} expected 200, got {status} body={payload.decode('utf-8', errors='replace')}")
    if "application/json" not in content_type:
        raise RuntimeError(f"{path} expected application/json, got {content_type}")
    return json.loads(payload.decode("utf-8"))


def request_json_http(base_url: str, path: str, method: str = "GET", body: object | None = None) -> tuple[int, dict | None]:
    status, content_type, payload = http_request(base_url, path, method=method, body=body)
    decoded = None
    if payload:
        if "application/json" not in content_type:
            raise RuntimeError(f"{path} expected application/json for JSON request, got {content_type}")
        decoded = json.loads(payload.decode("utf-8"))
    return status, decoded


def wait_for_http_ok(base_url: str, path: str, timeout_seconds: float) -> None:
    deadline = time.time() + timeout_seconds
    last_error = "not started"
    while time.time() < deadline:
        try:
            status, _, _ = http_request(base_url, path)
            if status == 200:
                return
            last_error = f"status={status}"
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for {path}: {last_error}")


def wait_for_json_ready(base_url: str, path: str, timeout_seconds: float) -> dict:
    deadline = time.time() + timeout_seconds
    last_error = "not started"
    while time.time() < deadline:
        try:
            return load_json_http(base_url, path)
        except Exception as exc:
            last_error = str(exc)
            time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for {path}: {last_error}")


class PortForward:
    def __init__(
        self,
        cluster_name: str,
        namespace: str,
        resource_name: str,
        remote_port: int,
        *,
        resource_kind: str = "service",
    ) -> None:
        self._cluster_name = cluster_name
        self._namespace = namespace
        self._resource_name = resource_name
        self._resource_kind = resource_kind
        self._remote_port = remote_port
        self.local_port = choose_free_port()
        self._process: subprocess.Popen[str] | None = None
        self._log_path: Path | None = None
        self._log_handle = None

    def __enter__(self) -> "PortForward":
        temp_dir = Path(tempfile.mkdtemp(prefix="dynaxis-kind-port-forward-"))
        log_stem = f"{self._resource_kind}-{self._resource_name}"
        self._log_path = temp_dir / f"{log_stem}.log"
        self._log_handle = self._log_path.open("w", encoding="utf-8")
        command = [
            "kubectl",
            "--context",
            f"kind-{self._cluster_name}",
            "--namespace",
            self._namespace,
            "port-forward",
            f"{self._resource_kind}/{self._resource_name}",
            f"{self.local_port}:{self._remote_port}",
        ]
        self._process = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            stdout=self._log_handle,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._process is not None and self._process.poll() is None:
            self._process.terminate()
            try:
                self._process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=10)
        if self._log_handle is not None:
            self._log_handle.close()

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.local_port}"

    def assert_running(self) -> None:
        if self._process is not None and self._process.poll() is not None:
            raise RuntimeError(
                f"kubectl port-forward exited early with code {self._process.returncode}: {self.read_log()}"
            )

    def read_log(self) -> str:
        if self._log_path is None or not self._log_path.exists():
            return ""
        return self._log_path.read_text(encoding="utf-8", errors="replace")


def wait_for_port_forward(port_forward: PortForward, timeout_seconds: float) -> str:
    deadline = time.time() + timeout_seconds
    last_error = "port-forward not ready"
    while time.time() < deadline:
        try:
            port_forward.assert_running()
            wait_for_http_ok(port_forward.base_url, "/healthz", timeout_seconds=1.0)
            return port_forward.base_url
        except Exception as exc:
            last_error = str(exc)
            time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for admin-app port-forward: {last_error}")


def load_file_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def cleanup_control_plane_documents(base_url: str) -> None:
    for path in (
        "/api/v1/topology/actuation/runtime-assignment",
        "/api/v1/topology/actuation/adapter",
        "/api/v1/topology/actuation/execution",
        "/api/v1/topology/actuation/request",
        "/api/v1/topology/desired",
    ):
        status, payload = request_json_http(base_url, path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"cleanup DELETE failed: {path} status={status} payload={payload}")


def find_current_world(observed_topology: dict) -> tuple[dict, dict, dict]:
    worlds = observed_topology.get("data", {}).get("worlds", [])
    instances = observed_topology.get("data", {}).get("instances", [])
    if not isinstance(worlds, list) or not worlds:
        raise RuntimeError("observed topology worlds[] missing")
    if not isinstance(instances, list) or not instances:
        raise RuntimeError("observed topology instances[] missing")

    server_instances = [
        item
        for item in instances
        if isinstance(item, dict)
        and item.get("role") == "server"
        and item.get("ready") is True
        and isinstance(item.get("world_scope"), dict)
        and item.get("world_scope", {}).get("world_id")
    ]
    if len(server_instances) < 2:
        raise RuntimeError("observed topology should expose at least two ready server instances")

    current_world = sorted(worlds, key=lambda item: str(item.get("world_id")))[0]
    current_world_id = str(current_world.get("world_id") or "")
    if not current_world_id:
        raise RuntimeError("selected world is missing world_id")

    current_owner_instance_id = str(current_world.get("owner_instance_id") or "")
    source_instance = next(
        (
            item
            for item in server_instances
            if item.get("instance_id") == current_owner_instance_id
        ),
        None,
    )
    if not isinstance(source_instance, dict):
        source_instance = next(
            (
                item
                for item in server_instances
                if item.get("world_scope", {}).get("world_id") == current_world_id
            ),
            None,
        )
    if not isinstance(source_instance, dict):
        raise RuntimeError("could not find source server instance for selected world")

    target_instance = next(
        (
            item
            for item in server_instances
            if item.get("world_scope", {}).get("world_id") != current_world_id
            and int(item.get("active_sessions") or 0) == 0
        ),
        None,
    )
    if not isinstance(target_instance, dict):
        raise RuntimeError("could not find an idle target server instance in another world")

    return current_world, source_instance, target_instance


def find_scale_out_action(actuation: dict, world_id: str, shard: str) -> dict:
    actions = actuation.get("data", {}).get("actions", [])
    for action in actions:
        if (
            isinstance(action, dict)
            and action.get("world_id") == world_id
            and action.get("shard") == shard
            and action.get("action") == "scale_out_pool"
        ):
            return action
    raise RuntimeError(f"scale_out_pool action not found for world={world_id} shard={shard}")


def wait_for_assignment_satisfied(base_url: str, timeout_seconds: float) -> dict:
    deadline = time.time() + timeout_seconds
    last_payload: dict | None = None
    while time.time() < deadline:
        payload = load_json_http(base_url, "/api/v1/topology/actuation/status?timeout_ms=5000")
        last_payload = payload
        summary = payload.get("data", {}).get("summary", {})
        if summary.get("satisfied_actions") == 1:
            return payload
        time.sleep(0.5)
    raise RuntimeError(f"runtime assignment was not satisfied before timeout: {last_payload}")


def wait_for_world_instances(base_url: str, world_id: str, expected_instances: int, timeout_seconds: float) -> dict:
    deadline = time.time() + timeout_seconds
    last_payload: dict | None = None
    while time.time() < deadline:
        payload = load_json_http(base_url, "/api/v1/topology/observed?timeout_ms=5000")
        last_payload = payload
        worlds = payload.get("data", {}).get("worlds", [])
        current_world = next((item for item in worlds if item.get("world_id") == world_id), None)
        if isinstance(current_world, dict) and len(current_world.get("instances", [])) == expected_instances:
            return payload
        time.sleep(0.5)
    raise RuntimeError(f"observed topology did not reach expected world size: {last_payload}")


def wait_for_observed_topology(base_url: str, timeout_seconds: float) -> dict:
    deadline = time.time() + timeout_seconds
    last_payload: dict | None = None
    last_error = "not started"
    while time.time() < deadline:
        try:
            payload = load_json_http(base_url, "/api/v1/topology/observed?timeout_ms=5000")
            last_payload = payload
            data = payload.get("data", {})
            if data.get("instances") and data.get("worlds"):
                return payload
            last_error = f"incomplete observed topology payload: {payload}"
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.5)
    raise RuntimeError(
        f"timeout waiting for observed topology readiness: {last_error} last_payload={last_payload}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run an optional live kind control-plane proof for topology actuation and runtime assignment."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    cluster_name = f"dynaxis-control-plane-{int(time.time())}"
    namespace = "dynaxis-control-plane"

    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-control-plane-") as temp_dir_raw:
        output_dir = Path(temp_dir_raw)
        summary_path = output_dir / "summary.json"
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
            summary = load_file_json(summary_path)

            with PortForward(cluster_name, namespace, "admin-app", ADMIN_SERVICE_PORT) as admin_forward:
                base_url = wait_for_port_forward(admin_forward, timeout_seconds=30.0)
                wait_for_http_ok(base_url, "/readyz", timeout_seconds=15.0)

                auth_context = wait_for_json_ready(base_url, "/api/v1/auth/context", timeout_seconds=10.0)
                auth_data = auth_context.get("data", {})
                if auth_data.get("mode") != "off":
                    raise RuntimeError("admin auth mode mismatch for kind control-plane proof")
                if auth_data.get("read_only") is not False:
                    raise RuntimeError("admin read_only should be false for kind control-plane proof")
                capabilities = auth_data.get("capabilities", {})
                for capability in (
                    "desired_topology",
                    "topology_actuation_request",
                    "topology_actuation_execution",
                    "topology_actuation_adapter",
                    "topology_actuation_runtime_assignment",
                ):
                    if capabilities.get(capability) is not True:
                        raise RuntimeError(f"admin capability is not writable: {capability}")

                observed_topology = wait_for_observed_topology(base_url, timeout_seconds=30.0)
                observed_summary = observed_topology.get("data", {}).get("summary", {})
                if int(observed_summary.get("worlds_total") or 0) != len(summary.get("server_workloads", [])):
                    raise RuntimeError("observed topology world count mismatch")

                current_world, source_instance, target_instance = find_current_world(observed_topology)
                world_id = str(current_world.get("world_id"))
                shard = str(source_instance.get("shard") or "")
                if not shard:
                    raise RuntimeError("source server instance is missing shard")

                cleanup_control_plane_documents(base_url)

                actuation_initial = wait_for_json_ready(
                    base_url,
                    "/api/v1/topology/actuation?timeout_ms=5000",
                    timeout_seconds=20.0,
                )
                initial_summary = actuation_initial.get("data", {}).get("summary", {})
                if initial_summary.get("desired_present") is not False:
                    raise RuntimeError("topology actuation should report desired_present=false before PUT")
                if int(initial_summary.get("observe_only_actions") or 0) < 1:
                    raise RuntimeError("topology actuation should expose observe-only actions before desired topology PUT")

                desired_topology_body = {
                    "topology_id": "kind-runtime-assignment-topology",
                    "pools": [
                        {
                            "world_id": world_id,
                            "shard": shard,
                            "replicas": 2,
                            "capacity_class": "kind",
                            "placement_tags": ["env:localdev", "orchestrator:kind"],
                        }
                    ],
                }
                status, payload = request_json_http(
                    base_url,
                    "/api/v1/topology/desired",
                    method="PUT",
                    body=desired_topology_body,
                )
                if status != 200:
                    raise RuntimeError(f"desired topology PUT failed: status={status} payload={payload}")
                desired_document = (payload or {}).get("data", {}).get("topology", {})
                if desired_document.get("revision") != 1:
                    raise RuntimeError("desired topology revision mismatch")

                actuation_after_desired = wait_for_json_ready(
                    base_url,
                    "/api/v1/topology/actuation?timeout_ms=5000",
                    timeout_seconds=20.0,
                )
                scale_out_action = find_scale_out_action(actuation_after_desired, world_id, shard)
                if scale_out_action.get("actionable") is not True:
                    raise RuntimeError("scale_out_pool action should be actionable")
                if int(scale_out_action.get("replica_delta") or 0) != 1:
                    raise RuntimeError("scale_out_pool replica_delta mismatch")

                request_body = {
                    "request_id": "kind-runtime-assignment-request",
                    "basis_topology_revision": 1,
                    "actions": [
                        {
                            "world_id": world_id,
                            "shard": shard,
                            "action": "scale_out_pool",
                            "replica_delta": 1,
                        }
                    ],
                }
                status, payload = request_json_http(
                    base_url,
                    "/api/v1/topology/actuation/request",
                    method="PUT",
                    body=request_body,
                )
                if status != 200:
                    raise RuntimeError(f"actuation request PUT failed: status={status} payload={payload}")
                request_document = (payload or {}).get("data", {}).get("request", {})
                if request_document.get("revision") != 1:
                    raise RuntimeError("actuation request revision mismatch")

                execution_body = {
                    "executor_id": "kind-runtime-assignment-executor",
                    "request_revision": 1,
                    "actions": [
                        {
                            "world_id": world_id,
                            "shard": shard,
                            "action": "scale_out_pool",
                            "replica_delta": 1,
                            "observed_instances_before": 1,
                            "ready_instances_before": 1,
                            "state": "claimed",
                        }
                    ],
                }
                status, payload = request_json_http(
                    base_url,
                    "/api/v1/topology/actuation/execution",
                    method="PUT",
                    body=execution_body,
                )
                if status != 200:
                    raise RuntimeError(f"actuation execution PUT failed: status={status} payload={payload}")
                execution_document = (payload or {}).get("data", {}).get("execution", {})
                if execution_document.get("revision") != 1:
                    raise RuntimeError("actuation execution revision mismatch")

                adapter_body = {
                    "adapter_id": "kind-runtime-assignment-adapter",
                    "execution_revision": 1,
                    "actions": [
                        {
                            "world_id": world_id,
                            "shard": shard,
                            "action": "scale_out_pool",
                            "replica_delta": 1,
                        }
                    ],
                }
                status, payload = request_json_http(
                    base_url,
                    "/api/v1/topology/actuation/adapter",
                    method="PUT",
                    body=adapter_body,
                )
                if status != 200:
                    raise RuntimeError(f"actuation adapter PUT failed: status={status} payload={payload}")
                adapter_document = (payload or {}).get("data", {}).get("lease", {})
                if adapter_document.get("revision") != 1:
                    raise RuntimeError("actuation adapter revision mismatch")

                runtime_assignment_body = {
                    "adapter_id": "kind-runtime-assignment-adapter",
                    "lease_revision": 1,
                    "assignments": [
                        {
                            "instance_id": str(target_instance.get("instance_id")),
                            "world_id": world_id,
                            "shard": shard,
                            "action": "scale_out_pool",
                        }
                    ],
                }
                status, payload = request_json_http(
                    base_url,
                    "/api/v1/topology/actuation/runtime-assignment",
                    method="PUT",
                    body=runtime_assignment_body,
                )
                if status != 200:
                    raise RuntimeError(f"runtime assignment PUT failed: status={status} payload={payload}")
                assignment_document = (payload or {}).get("data", {}).get("assignment", {})
                if assignment_document.get("revision") != 1:
                    raise RuntimeError("runtime assignment revision mismatch")

                wait_for_assignment_satisfied(base_url, timeout_seconds=20.0)
                wait_for_world_instances(base_url, world_id, expected_instances=2, timeout_seconds=20.0)

                status, payload = request_json_http(
                    base_url,
                    "/api/v1/topology/actuation/execution",
                    method="PUT",
                    body={
                        **execution_body,
                        "expected_revision": 1,
                        "actions": [
                            {
                                "world_id": world_id,
                                "shard": shard,
                                "action": "scale_out_pool",
                                "replica_delta": 1,
                                "observed_instances_before": 1,
                                "ready_instances_before": 1,
                                "state": "completed",
                            }
                        ],
                    },
                )
                if status != 200:
                    raise RuntimeError(f"actuation execution completion PUT failed: status={status} payload={payload}")
                if ((payload or {}).get("data", {}).get("execution", {}) or {}).get("revision") != 2:
                    raise RuntimeError("actuation execution completion revision mismatch")

                status, payload = request_json_http(
                    base_url,
                    "/api/v1/topology/actuation/adapter",
                    method="PUT",
                    body={
                        **adapter_body,
                        "execution_revision": 2,
                        "expected_revision": 1,
                    },
                )
                if status != 200:
                    raise RuntimeError(f"actuation adapter refresh PUT failed: status={status} payload={payload}")
                if ((payload or {}).get("data", {}).get("lease", {}) or {}).get("revision") != 2:
                    raise RuntimeError("actuation adapter refresh revision mismatch")

                adapter_status = wait_for_json_ready(
                    base_url,
                    "/api/v1/topology/actuation/adapter/status?timeout_ms=5000",
                    timeout_seconds=20.0,
                )
                adapter_status_summary = adapter_status.get("data", {}).get("summary", {})
                if adapter_status_summary.get("realized_actions") != 1:
                    raise RuntimeError("adapter status should report one realized action")

                cleanup_control_plane_documents(base_url)

        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="", file=sys.stderr)

    print("PASS worlds-kubernetes-kind-control-plane")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
