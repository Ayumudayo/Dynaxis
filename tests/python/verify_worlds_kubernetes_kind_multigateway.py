from __future__ import annotations

import argparse
import sys
import tempfile
import time

from verify_worlds_kubernetes_kind import DEFAULT_TOPOLOGY
from verify_worlds_kubernetes_kind import RUNNER
from verify_worlds_kubernetes_kind import detect_prerequisites
from verify_worlds_kubernetes_kind import run
from verify_worlds_kubernetes_kind import skip
from verify_worlds_kubernetes_kind_closure import SAME_WORLD_TOPOLOGY
from verify_worlds_kubernetes_kind_continuity import (
    ADMIN_SERVICE_PORT,
    GATEWAY_SERVICE_PORT,
    CONTINUITY_WORLD_OWNER_PREFIX,
    SESSION_DIRECTORY_PREFIX,
    acquire_session_for_backend,
    assert_initial_login,
    assert_world_capabilities,
    current_backend_for_resume_token,
    delete_world_endpoint,
    make_resume_routing_key,
    resume_until_success,
    select_default_worlds,
    select_same_world_transfer_source,
    wait_for_redis_value,
    wait_for_world_endpoint,
    wait_for_world_inventory,
)
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_control_plane import request_json_http
from verify_worlds_kubernetes_kind_control_plane import wait_for_http_ok
from verify_worlds_kubernetes_kind_control_plane import wait_for_observed_topology
from verify_worlds_kubernetes_kind_control_plane import wait_for_port_forward
from verify_worlds_kubernetes_kind_control_plane import wait_for_json_ready


def wait_for_tcp_port_forward(port_forward: PortForward, timeout_seconds: float) -> tuple[str, int]:
    import socket

    deadline = time.time() + timeout_seconds
    last_error = "port-forward not ready"
    while time.time() < deadline:
        try:
            port_forward.assert_running()
            with socket.create_connection(("127.0.0.1", port_forward.local_port), timeout=1.0):
                return "127.0.0.1", int(port_forward.local_port)
        except Exception as exc:
            last_error = str(exc)
            time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for gateway port-forward: {last_error}")


def run_default_cross_gateway_resume(
    cluster_name: str,
    namespace: str,
    base_url: str,
    login_host: str,
    login_port: int,
    resume_host: str,
    resume_port: int,
) -> None:
    from session_continuity_common import MSG_CHAT_SEND
    from session_continuity_common import lp_utf8

    observed_topology = wait_for_observed_topology(base_url, timeout_seconds=30.0)
    observed_topology = wait_for_world_endpoint(
        base_url,
        "/api/v1/topology/observed?timeout_ms=5000",
        lambda payload: int((payload.get("data", {}).get("summary", {}) or {}).get("worlds_total") or 0) >= 2,
        timeout_seconds=20.0,
    )
    source_world, target_world = select_default_worlds(observed_topology)
    source_world_id = str(source_world["world_id"])
    target_world_id = str(target_world["world_id"])
    source_instance_id = str(source_world["instances"][0]["instance_id"])
    target_owner_instance_id = str(target_world["instances"][0]["instance_id"])
    room = f"kind_cross_gateway_migration_room_{int(time.time())}"
    message = f"kind_cross_gateway_migration_msg_{int(time.time() * 1000)}"

    first = None
    second = None
    drain_path = f"/api/v1/worlds/{source_world_id}/drain"
    migration_path = f"/api/v1/worlds/{source_world_id}/migration"
    try:
        first, login, user = acquire_session_for_backend(
            cluster_name,
            namespace,
            source_instance_id,
            login_host,
            login_port,
            "kind_cross_gateway_migration",
        )
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login.get("world_id") != source_world_id:
            raise RuntimeError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        alias_key = f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}"
        if wait_for_redis_value(cluster_name, namespace, alias_key) != source_instance_id:
            raise RuntimeError(f"resume alias was not attached to {source_instance_id} before migration proof")

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

        first.close()
        first = None

        wait_for_world_endpoint(
            base_url,
            migration_path,
            lambda candidate: (candidate.get("data", {}).get("migration", {}) or {}).get("phase") == "ready_to_resume",
            timeout_seconds=20.0,
        )
        wait_for_world_endpoint(
            base_url,
            drain_path,
            lambda candidate: (
                (candidate.get("data", {}).get("drain", {}) or {}).get("phase") == "drained"
                and ((candidate.get("data", {}).get("drain", {}) or {}).get("orchestration", {}) or {}).get("phase")
                == "ready_to_clear"
            ),
            timeout_seconds=20.0,
        )

        second, resumed = resume_until_success(
            cluster_name,
            namespace,
            resume_host,
            resume_port,
            resume_token,
            user,
            logical_session_id,
        )
        resumed_backend = current_backend_for_resume_token(cluster_name, namespace, resume_token)
        if resumed_backend != target_owner_instance_id:
            raise RuntimeError(f"resume routing did not bind to the migration target backend: {resumed_backend!r}")
        if resumed.get("world_id") != target_world_id:
            raise RuntimeError(f"resume did not migrate to world {target_world_id}: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        delete_world_endpoint(base_url, drain_path)
        delete_world_endpoint(base_url, migration_path)

        print(
            "PASS kind-default-cross-gateway-resume: "
            f"logical_session_id={logical_session_id} login_gateway=gateway-1 resume_gateway=gateway-2 "
            f"resumed_backend={resumed_backend} world_id={target_world_id}"
        )
    finally:
        if first is not None:
            first.close()
        if second is not None:
            second.close()
        try:
            delete_world_endpoint(base_url, drain_path)
        except Exception:
            pass
        try:
            delete_world_endpoint(base_url, migration_path)
        except Exception:
            pass


def run_same_world_cross_gateway_resume(
    cluster_name: str,
    namespace: str,
    base_url: str,
    login_host: str,
    login_port: int,
    resume_host: str,
    resume_port: int,
) -> None:
    from session_continuity_common import MSG_CHAT_SEND
    from session_continuity_common import lp_utf8

    worlds_payload = wait_for_world_inventory(
        base_url,
        lambda payload: any(
            isinstance(world, dict)
            and len(
                [
                    item
                    for item in world.get("instances", [])
                    if isinstance(item, dict) and item.get("instance_id") and item.get("ready") is True
                ]
            )
            >= 2
            for world in payload.get("data", {}).get("items", [])
        ),
        timeout_seconds=20.0,
    )
    source_world, source_instance_id, replacement_owner_instance_id = select_same_world_transfer_source(worlds_payload)
    source_world_id = str(source_world["world_id"])
    room = f"kind_cross_gateway_transfer_room_{int(time.time())}"
    message = f"kind_cross_gateway_transfer_msg_{int(time.time() * 1000)}"
    world_owner_key = f"{CONTINUITY_WORLD_OWNER_PREFIX}{source_world_id}"

    first = None
    second = None
    drain_path = f"/api/v1/worlds/{source_world_id}/drain"
    transfer_path = f"/api/v1/worlds/{source_world_id}/transfer"
    try:
        first, login, user = acquire_session_for_backend(
            cluster_name,
            namespace,
            source_instance_id,
            login_host,
            login_port,
            "kind_cross_gateway_transfer",
        )
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login.get("world_id") != source_world_id:
            raise RuntimeError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        alias_key = f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}"
        if wait_for_redis_value(cluster_name, namespace, alias_key) != source_instance_id:
            raise RuntimeError(f"resume alias was not attached to {source_instance_id} before transfer proof")
        if wait_for_redis_value(cluster_name, namespace, world_owner_key) != source_instance_id:
            raise RuntimeError(f"world owner key was not attached to {source_instance_id} before transfer proof")

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

        first.close()
        first = None

        wait_for_world_endpoint(
            base_url,
            drain_path,
            lambda candidate: (
                (candidate.get("data", {}).get("drain", {}) or {}).get("phase") == "drained"
                and ((candidate.get("data", {}).get("drain", {}) or {}).get("orchestration", {}) or {}).get("phase")
                == "awaiting_owner_transfer"
            ),
            timeout_seconds=20.0,
        )

        status, payload = request_json_http(
            base_url,
            transfer_path,
            method="PUT",
            body={
                "target_owner_instance_id": replacement_owner_instance_id,
                "expected_owner_instance_id": source_instance_id,
                "commit_owner": True,
            },
        )
        if status != 200:
            raise RuntimeError(f"world transfer PUT failed: status={status} payload={payload}")

        wait_for_world_endpoint(
            base_url,
            drain_path,
            lambda candidate: (
                (candidate.get("data", {}).get("drain", {}) or {}).get("phase") == "drained"
                and ((candidate.get("data", {}).get("drain", {}) or {}).get("orchestration", {}) or {}).get("phase")
                == "ready_to_clear"
            ),
            timeout_seconds=20.0,
        )

        if wait_for_redis_value(cluster_name, namespace, world_owner_key) != replacement_owner_instance_id:
            raise RuntimeError(
                f"world owner key was not committed to replacement backend {replacement_owner_instance_id}"
            )

        second, resumed = resume_until_success(
            cluster_name,
            namespace,
            resume_host,
            resume_port,
            resume_token,
            user,
            logical_session_id,
        )
        resumed_backend = current_backend_for_resume_token(cluster_name, namespace, resume_token)
        if resumed_backend != replacement_owner_instance_id:
            raise RuntimeError(
                f"resume routing did not bind to committed replacement backend: {resumed_backend!r}"
            )
        if resumed.get("world_id") != source_world_id:
            raise RuntimeError(f"resume did not preserve same-world residency after committed transfer: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        delete_world_endpoint(base_url, drain_path)
        delete_world_endpoint(base_url, transfer_path)

        print(
            "PASS kind-same-world-cross-gateway-resume: "
            f"logical_session_id={logical_session_id} login_gateway=gateway-1 resume_gateway=gateway-2 "
            f"resumed_backend={resumed_backend} world_id={source_world_id}"
        )
    finally:
        if first is not None:
            first.close()
        if second is not None:
            second.close()
        try:
            delete_world_endpoint(base_url, drain_path)
        except Exception:
            pass
        try:
            delete_world_endpoint(base_url, transfer_path)
        except Exception:
            pass


def run_stage(
    *,
    cluster_name: str,
    namespace: str,
    topology_config,
    scenario_name: str,
    timeout_seconds: int,
) -> None:
    with tempfile.TemporaryDirectory(prefix=f"dynaxis-kind-multigateway-{scenario_name}-") as temp_dir_raw:
        output_dir = temp_dir_raw
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
            output_dir,
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
                with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway1_forward:
                    with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT) as gateway2_forward:
                        base_url = wait_for_port_forward(admin_forward, timeout_seconds=30.0)
                        login_host, login_port = wait_for_tcp_port_forward(gateway1_forward, timeout_seconds=30.0)
                        resume_host, resume_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                        wait_for_http_ok(base_url, "/readyz", timeout_seconds=15.0)
                        assert_world_capabilities(base_url)
                        wait_for_observed_topology(base_url, timeout_seconds=30.0)

                        if scenario_name == "default-cross-gateway-resume":
                            run_default_cross_gateway_resume(
                                cluster_name,
                                namespace,
                                base_url,
                                login_host,
                                login_port,
                                resume_host,
                                resume_port,
                            )
                        elif scenario_name == "same-world-cross-gateway-resume":
                            run_same_world_cross_gateway_resume(
                                cluster_name,
                                namespace,
                                base_url,
                                login_host,
                                login_port,
                                resume_host,
                                resume_port,
                            )
                        else:
                            raise RuntimeError(f"unknown scenario: {scenario_name}")
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run topology-aware cross-gateway continuity proofs on live kind clusters."
    )
    parser.add_argument(
        "--scenario",
        choices=("matrix", "default-cross-gateway-resume", "same-world-cross-gateway-resume"),
        default="matrix",
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    try:
        if args.scenario in {"matrix", "default-cross-gateway-resume"}:
            run_stage(
                cluster_name=f"dynaxis-multigw-default-{int(time.time())}",
                namespace="dynaxis-multigw-default",
                topology_config=DEFAULT_TOPOLOGY,
                scenario_name="default-cross-gateway-resume",
                timeout_seconds=args.wait_timeout_seconds,
            )
        if args.scenario in {"matrix", "same-world-cross-gateway-resume"}:
            run_stage(
                cluster_name=f"dynaxis-multigw-sameworld-{int(time.time())}",
                namespace="dynaxis-multigw-sameworld",
                topology_config=SAME_WORLD_TOPOLOGY,
                scenario_name="same-world-cross-gateway-resume",
                timeout_seconds=args.wait_timeout_seconds,
            )
        print("PASS worlds-kubernetes-kind-multigateway")
        return 0
    except Exception as exc:
        print(f"FAIL worlds-kubernetes-kind-multigateway: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
