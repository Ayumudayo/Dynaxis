from __future__ import annotations

import argparse
import sys
import tempfile
import time
from pathlib import Path

from verify_worlds_kubernetes_kind import DEFAULT_TOPOLOGY
from verify_worlds_kubernetes_kind import RUNNER
from verify_worlds_kubernetes_kind import detect_prerequisites
from verify_worlds_kubernetes_kind import run
from verify_worlds_kubernetes_kind import skip
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_control_plane import load_json_http
from verify_worlds_kubernetes_kind_control_plane import request_json_http
from verify_worlds_kubernetes_kind_control_plane import wait_for_http_ok
from verify_worlds_kubernetes_kind_control_plane import wait_for_observed_topology
from verify_worlds_kubernetes_kind_control_plane import wait_for_port_forward
from verify_worlds_kubernetes_kind_control_plane import wait_for_json_ready


REPO_ROOT = Path(__file__).resolve().parents[2]
SAME_WORLD_TOPOLOGY = REPO_ROOT / "docker" / "stack" / "topologies" / "mmorpg-same-world-proof.json"
ADMIN_SERVICE_PORT = 39200


def wait_for_world_endpoint(
    base_url: str,
    path: str,
    predicate,
    *,
    timeout_seconds: float,
) -> dict:
    deadline = time.time() + timeout_seconds
    last_payload: dict | None = None
    while time.time() < deadline:
        payload = load_json_http(base_url, path)
        last_payload = payload
        if predicate(payload):
            return payload
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for {path}: {last_payload}")


def delete_world_endpoint(base_url: str, path: str) -> None:
    status, payload = request_json_http(base_url, path, method="DELETE")
    if status != 200:
        raise RuntimeError(f"DELETE {path} failed: status={status} payload={payload}")


def assert_world_capabilities(base_url: str) -> None:
    auth_context = wait_for_json_ready(base_url, "/api/v1/auth/context", timeout_seconds=10.0)
    auth_data = auth_context.get("data", {})
    if auth_data.get("mode") != "off":
        raise RuntimeError("admin auth mode mismatch for kind closure proof")
    if auth_data.get("read_only") is not False:
        raise RuntimeError("admin read_only should be false for kind closure proof")
    capabilities = auth_data.get("capabilities", {})
    for capability in ("world_drain", "world_transfer", "world_migration"):
        if capabilities.get(capability) is not True:
            raise RuntimeError(f"admin capability is not writable: {capability}")


def select_default_migration_worlds(observed_topology: dict) -> tuple[dict, dict]:
    worlds = observed_topology.get("data", {}).get("worlds", [])
    ready_worlds = [
        item
        for item in worlds
        if isinstance(item, dict)
        and item.get("world_id")
        and item.get("instances")
    ]
    if len(ready_worlds) < 2:
        raise RuntimeError("default topology closure proof requires at least two observed worlds")
    ready_worlds = sorted(ready_worlds, key=lambda item: str(item.get("world_id")))
    return ready_worlds[0], ready_worlds[1]


def select_same_world_transfer(worlds_payload: dict) -> tuple[dict, str]:
    worlds = worlds_payload.get("data", {}).get("items", [])
    for world in worlds:
        instances = world.get("instances", [])
        ready_instances = [
            item
            for item in instances
            if isinstance(item, dict) and item.get("instance_id") and item.get("ready") is True
        ]
        if len(ready_instances) >= 2:
            return world, str(ready_instances[1]["instance_id"])
    raise RuntimeError("same-world closure proof requires a world with at least two ready server instances")


def run_default_migration_closure(base_url: str) -> None:
    observed_topology = wait_for_observed_topology(base_url, timeout_seconds=30.0)
    source_world, target_world = select_default_migration_worlds(observed_topology)
    source_world_id = str(source_world["world_id"])
    target_world_id = str(target_world["world_id"])
    target_instances = target_world.get("instances", [])
    target_owner_instance_id = str(target_instances[0]["instance_id"])

    drain_path = f"/api/v1/worlds/{source_world_id}/drain"
    migration_path = f"/api/v1/worlds/{source_world_id}/migration"

    try:
        status, payload = request_json_http(
            base_url,
            migration_path,
            method="PUT",
            body={
                "target_world_id": target_world_id,
                "target_owner_instance_id": target_owner_instance_id,
                "preserve_room": True,
            },
        )
        if status != 200:
            raise RuntimeError(f"world migration PUT failed: status={status} payload={payload}")
        migration_state = (payload or {}).get("data", {}).get("migration", {})
        if migration_state.get("phase") != "awaiting_source_drain":
            raise RuntimeError(f"world migration phase mismatch after PUT: {payload}")

        status, payload = request_json_http(
            base_url,
            drain_path,
            method="PUT",
            body={
                "replacement_owner_instance_id": None,
            },
        )
        if status != 200:
            raise RuntimeError(f"world drain PUT failed: status={status} payload={payload}")

        ready_migration = wait_for_world_endpoint(
            base_url,
            migration_path,
            lambda candidate: (candidate.get("data", {}).get("migration", {}) or {}).get("phase") == "ready_to_resume",
            timeout_seconds=20.0,
        )
        ready_drain = wait_for_world_endpoint(
            base_url,
            drain_path,
            lambda candidate: (
                (candidate.get("data", {}).get("drain", {}) or {}).get("phase") == "drained"
                and ((candidate.get("data", {}).get("drain", {}) or {}).get("orchestration", {}) or {}).get("phase")
                == "ready_to_clear"
            ),
            timeout_seconds=20.0,
        )

        orchestration = (ready_drain.get("data", {}).get("drain", {}) or {}).get("orchestration", {})
        summary = orchestration.get("summary", {})
        if orchestration.get("target_world_id") != target_world_id:
            raise RuntimeError(f"world drain target_world_id mismatch: {ready_drain}")
        if orchestration.get("target_owner_instance_id") != target_owner_instance_id:
            raise RuntimeError(f"world drain target_owner_instance_id mismatch: {ready_drain}")
        if summary.get("migration_declared") is not True:
            raise RuntimeError(f"world drain did not retain migration_declared summary: {ready_drain}")
        if summary.get("migration_ready") is not True:
            raise RuntimeError(f"world drain did not report migration_ready: {ready_drain}")
        if summary.get("clear_allowed") is not True:
            raise RuntimeError(f"world drain did not allow clear after migration readiness: {ready_drain}")

        if (ready_migration.get("data", {}).get("migration", {}) or {}).get("target_world_id") != target_world_id:
            raise RuntimeError(f"world migration target_world_id mismatch: {ready_migration}")

        delete_world_endpoint(base_url, drain_path)
        delete_world_endpoint(base_url, migration_path)
    finally:
        try:
            delete_world_endpoint(base_url, drain_path)
        except Exception:
            pass
        try:
            delete_world_endpoint(base_url, migration_path)
        except Exception:
            pass

    print(
        "PASS kind-default-migration-closure: "
        f"source_world={source_world_id} target_world={target_world_id} target_owner={target_owner_instance_id}"
    )


def run_same_world_transfer_closure(base_url: str) -> None:
    worlds_payload = load_json_http(base_url, "/api/v1/worlds?limit=100")
    source_world, replacement_owner_instance_id = select_same_world_transfer(worlds_payload)
    source_world_id = str(source_world["world_id"])
    drain_path = f"/api/v1/worlds/{source_world_id}/drain"
    transfer_path = f"/api/v1/worlds/{source_world_id}/transfer"

    try:
        status, payload = request_json_http(
            base_url,
            drain_path,
            method="PUT",
            body={
                "replacement_owner_instance_id": replacement_owner_instance_id,
            },
        )
        if status != 200:
            raise RuntimeError(f"world drain PUT failed: status={status} payload={payload}")

        awaiting_transfer = wait_for_world_endpoint(
            base_url,
            drain_path,
            lambda candidate: (
                (candidate.get("data", {}).get("drain", {}) or {}).get("phase") == "drained"
                and ((candidate.get("data", {}).get("drain", {}) or {}).get("orchestration", {}) or {}).get("phase")
                == "awaiting_owner_transfer"
            ),
            timeout_seconds=20.0,
        )
        awaiting_orchestration = (awaiting_transfer.get("data", {}).get("drain", {}) or {}).get("orchestration", {})
        if awaiting_orchestration.get("next_action") != "commit_owner_transfer":
            raise RuntimeError(f"world drain next_action mismatch before transfer commit: {awaiting_transfer}")

        status, payload = request_json_http(
            base_url,
            transfer_path,
            method="PUT",
            body={
                "target_owner_instance_id": replacement_owner_instance_id,
                "commit_owner": True,
            },
        )
        if status != 200:
            raise RuntimeError(f"world transfer PUT failed: status={status} payload={payload}")
        transfer_data = (payload or {}).get("data", {})
        if transfer_data.get("owner_instance_id") != replacement_owner_instance_id:
            raise RuntimeError(f"world transfer owner_instance_id mismatch: {payload}")
        if transfer_data.get("owner_commit_applied") is not True:
            raise RuntimeError(f"world transfer owner_commit_applied mismatch: {payload}")
        if (transfer_data.get("transfer", {}) or {}).get("phase") != "owner_handoff_committed":
            raise RuntimeError(f"world transfer phase mismatch after commit: {payload}")

        ready_to_clear = wait_for_world_endpoint(
            base_url,
            drain_path,
            lambda candidate: (
                (candidate.get("data", {}).get("drain", {}) or {}).get("phase") == "drained"
                and ((candidate.get("data", {}).get("drain", {}) or {}).get("orchestration", {}) or {}).get("phase")
                == "ready_to_clear"
            ),
            timeout_seconds=20.0,
        )
        clear_orchestration = (ready_to_clear.get("data", {}).get("drain", {}) or {}).get("orchestration", {})
        clear_summary = clear_orchestration.get("summary", {})
        if clear_orchestration.get("next_action") != "clear_policy":
            raise RuntimeError(f"world drain next_action mismatch after transfer commit: {ready_to_clear}")
        if clear_summary.get("transfer_declared") is not True:
            raise RuntimeError(f"world drain did not report transfer_declared: {ready_to_clear}")
        if clear_summary.get("transfer_committed") is not True:
            raise RuntimeError(f"world drain did not report transfer_committed: {ready_to_clear}")

        delete_world_endpoint(base_url, drain_path)

        world_inventory_after_clear = load_json_http(base_url, "/api/v1/worlds?limit=100")
        updated_world = next(
            (
                item
                for item in world_inventory_after_clear.get("data", {}).get("items", [])
                if item.get("world_id") == source_world_id
            ),
            None,
        )
        if not isinstance(updated_world, dict):
            raise RuntimeError("world inventory entry disappeared after transfer closure")
        if updated_world.get("owner_instance_id") != replacement_owner_instance_id:
            raise RuntimeError(f"world owner did not remain committed after drain clear: {updated_world}")
        if (updated_world.get("drain", {}) or {}).get("phase") != "idle":
            raise RuntimeError(f"world drain did not clear to idle after closure: {updated_world}")
    finally:
        try:
            delete_world_endpoint(base_url, drain_path)
        except Exception:
            pass
        try:
            delete_world_endpoint(base_url, transfer_path)
        except Exception:
            pass

    print(
        "PASS kind-same-world-transfer-closure: "
        f"world_id={source_world_id} replacement_owner={replacement_owner_instance_id}"
    )


def run_stage(
    *,
    cluster_name: str,
    namespace: str,
    topology_config: Path,
    scenario_name: str,
    timeout_seconds: int,
) -> None:
    with tempfile.TemporaryDirectory(prefix=f"dynaxis-kind-closure-{scenario_name}-") as temp_dir_raw:
        output_dir = Path(temp_dir_raw)
        up_command = [
            sys.executable,
            str(RUNNER),
            "up",
            "--cluster-name",
            cluster_name,
            "--topology-config",
            str(topology_config),
            "--namespace",
            namespace,
            "--output-dir",
            str(output_dir),
            "--wait-timeout-seconds",
            str(timeout_seconds),
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
                wait_for_http_ok(base_url, "/readyz", timeout_seconds=15.0)
                assert_world_capabilities(base_url)
                wait_for_observed_topology(base_url, timeout_seconds=30.0)

                if scenario_name == "default-migration-closure":
                    run_default_migration_closure(base_url)
                elif scenario_name == "same-world-transfer-closure":
                    run_same_world_transfer_closure(base_url)
                else:
                    raise RuntimeError(f"unknown scenario: {scenario_name}")
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run topology-aware world lifecycle closure proofs on live kind clusters."
    )
    parser.add_argument(
        "--scenario",
        choices=("matrix", "default-migration-closure", "same-world-transfer-closure"),
        default="matrix",
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    try:
        if args.scenario in {"matrix", "default-migration-closure"}:
            run_stage(
                cluster_name=f"dynaxis-closure-default-{int(time.time())}",
                namespace="dynaxis-closure-default",
                topology_config=DEFAULT_TOPOLOGY,
                scenario_name="default-migration-closure",
                timeout_seconds=args.wait_timeout_seconds,
            )
        if args.scenario in {"matrix", "same-world-transfer-closure"}:
            run_stage(
                cluster_name=f"dynaxis-closure-sameworld-{int(time.time())}",
                namespace="dynaxis-closure-sameworld",
                topology_config=SAME_WORLD_TOPOLOGY,
                scenario_name="same-world-transfer-closure",
                timeout_seconds=args.wait_timeout_seconds,
            )
        print("PASS worlds-kubernetes-kind-closure")
        return 0
    except Exception as exc:
        print(f"FAIL worlds-kubernetes-kind-closure: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
