from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time

from session_continuity_common import ChatClient
from session_continuity_common import lp_utf8
from session_continuity_common import MSG_CHAT_SEND
from verify_worlds_kubernetes_kind import DEFAULT_TOPOLOGY
from verify_worlds_kubernetes_kind import RUNNER
from verify_worlds_kubernetes_kind import detect_prerequisites
from verify_worlds_kubernetes_kind import run
from verify_worlds_kubernetes_kind import skip
from verify_worlds_kubernetes_kind_continuity import (
    ADMIN_SERVICE_PORT,
    GATEWAY_SERVICE_PORT,
    SESSION_DIRECTORY_PREFIX,
    acquire_session_for_backend,
    assert_initial_login,
    current_backend_for_resume_token,
    current_backend_for_user,
    make_resume_routing_key,
    resume_until_success,
    select_default_worlds,
    wait_for_redis_value,
)
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_control_plane import wait_for_port_forward
from verify_worlds_kubernetes_kind_control_plane import wait_for_http_ok
from verify_worlds_kubernetes_kind_control_plane import wait_for_observed_topology


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


def get_pod_uids(cluster_name: str, namespace: str, selector: str) -> dict[str, str]:
    command = [
        "kubectl",
        "--context",
        f"kind-{cluster_name}",
        "--namespace",
        namespace,
        "get",
        "pods",
        "-l",
        selector,
        "-o",
        "json",
    ]
    result = run(command)
    payload = json.loads(result.stdout or "{}")
    out: dict[str, str] = {}
    for item in payload.get("items", []):
        metadata = item.get("metadata", {})
        name = metadata.get("name")
        uid = metadata.get("uid")
        if isinstance(name, str) and isinstance(uid, str):
            out[name] = uid
    return out


def rollout_restart(cluster_name: str, namespace: str, resource: str, timeout_seconds: int) -> None:
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "rollout",
            "restart",
            resource,
        ]
    )
    run(
        [
            "kubectl",
            "--context",
            f"kind-{cluster_name}",
            "--namespace",
            namespace,
            "rollout",
            "status",
            resource,
            f"--timeout={timeout_seconds}s",
        ]
    )


def wait_for_pod_uid_change(
    cluster_name: str,
    namespace: str,
    selector: str,
    previous_uids: dict[str, str],
    timeout_seconds: float,
) -> dict[str, str]:
    deadline = time.time() + timeout_seconds
    last_uids: dict[str, str] = {}
    while time.time() < deadline:
        current = get_pod_uids(cluster_name, namespace, selector)
        last_uids = current
        if current and current != previous_uids:
            return current
        time.sleep(0.5)
    raise RuntimeError(f"pod uid did not change for selector {selector}: before={previous_uids} after={last_uids}")


def run_gateway_restart_scenario(
    cluster_name: str,
    namespace: str,
    gateway1_host: str,
    gateway1_port: int,
    gateway2_host: str,
    gateway2_port: int,
    source_instance_id: str,
) -> None:
    room = f"kind_gateway_restart_room_{int(time.time())}"
    message = f"kind_gateway_restart_msg_{int(time.time() * 1000)}"

    first = None
    second = None
    try:
        first, login, user = acquire_session_for_backend(
            cluster_name,
            namespace,
            source_instance_id,
            gateway1_host,
            gateway1_port,
            "kind_gateway_restart",
        )
        logical_session_id, resume_token = assert_initial_login(login, user)
        first.join_room(room, user)

        alias_key = f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}"
        alias_backend_before = wait_for_redis_value(cluster_name, namespace, alias_key)
        if alias_backend_before != source_instance_id:
            raise RuntimeError(
                f"resume alias was not attached to {source_instance_id} before gateway restart: {alias_backend_before!r}"
            )

        gateway_selector = "app.kubernetes.io/name=gateway-1"
        gateway_uids_before = get_pod_uids(cluster_name, namespace, gateway_selector)
        rollout_restart(cluster_name, namespace, "deployment/gateway-1", 240)
        wait_for_pod_uid_change(cluster_name, namespace, gateway_selector, gateway_uids_before, 60.0)

        first.close()
        first = None

        second, resumed = resume_until_success(
            cluster_name,
            namespace,
            gateway2_host,
            gateway2_port,
            resume_token,
            user,
            logical_session_id,
        )
        resumed_backend = current_backend_for_resume_token(cluster_name, namespace, resume_token)
        if resumed_backend != alias_backend_before:
            raise RuntimeError(
                f"resume routing alias changed across gateway restart: before={alias_backend_before} after={resumed_backend}"
            )

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        print(
            "PASS kind-gateway-restart: "
            f"logical_session_id={logical_session_id} login_gateway=gateway-1 resume_gateway=gateway-2 "
            f"alias_backend={resumed_backend}"
        )
    finally:
        if first is not None:
            first.close()
        if second is not None:
            second.close()


def run_server_restart_scenario(
    cluster_name: str,
    namespace: str,
    gateway1_host: str,
    gateway1_port: int,
    gateway2_host: str,
    gateway2_port: int,
    source_instance_id: str,
    source_world_id: str,
) -> None:
    room = f"kind_server_restart_room_{int(time.time())}"
    message = f"kind_server_restart_msg_{int(time.time() * 1000)}"
    workload_name = source_instance_id.rsplit("-", 1)[0]
    statefulset_selector = f"app.kubernetes.io/name={workload_name}"

    first = None
    second = None
    try:
        first, login, user = acquire_session_for_backend(
            cluster_name,
            namespace,
            source_instance_id,
            gateway1_host,
            gateway1_port,
            "kind_server_restart",
        )
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login.get("world_id") != source_world_id:
            raise RuntimeError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        backend_before = current_backend_for_user(cluster_name, namespace, user)
        alias_before = current_backend_for_resume_token(cluster_name, namespace, resume_token)
        if backend_before != source_instance_id:
            raise RuntimeError(f"session was not attached to {source_instance_id} before restart: {backend_before!r}")
        if alias_before != source_instance_id:
            raise RuntimeError(f"resume alias was not attached to {source_instance_id} before restart: {alias_before!r}")

        server_uids_before = get_pod_uids(cluster_name, namespace, statefulset_selector)
        rollout_restart(cluster_name, namespace, f"statefulset/{workload_name}", 240)
        wait_for_pod_uid_change(cluster_name, namespace, statefulset_selector, server_uids_before, 60.0)

        first.close()
        first = None

        second, resumed = resume_until_success(
            cluster_name,
            namespace,
            gateway2_host,
            gateway2_port,
            resume_token,
            user,
            logical_session_id,
        )
        if resumed.get("world_id") != source_world_id:
            raise RuntimeError(f"resume did not preserve same-world residency after server restart: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        backend_after = current_backend_for_user(cluster_name, namespace, user)
        alias_after = current_backend_for_resume_token(cluster_name, namespace, resume_token)
        if not backend_after:
            raise RuntimeError("user backend routing was not restored after server restart")
        if not alias_after:
            raise RuntimeError("resume alias was not restored after server restart")

        print(
            "PASS kind-server-restart: "
            f"logical_session_id={logical_session_id} login_gateway=gateway-1 resume_gateway=gateway-2 "
            f"backend_before={backend_before} backend_after={backend_after}"
        )
    finally:
        if first is not None:
            first.close()
        if second is not None:
            second.close()


def run_stage(*, cluster_name: str, namespace: str, timeout_seconds: int) -> None:
    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-restart-") as temp_dir_raw:
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
            temp_dir_raw,
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
                observed_topology = wait_for_observed_topology(base_url, timeout_seconds=30.0)
                source_world, _target_world = select_default_worlds(observed_topology)
                source_world_id = str(source_world["world_id"])
                source_instance_id = str(source_world["instances"][0]["instance_id"])

                with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway1_forward:
                    with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT) as gateway2_forward:
                        gateway1_host, gateway1_port = wait_for_tcp_port_forward(gateway1_forward, timeout_seconds=30.0)
                        gateway2_host, gateway2_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                        run_gateway_restart_scenario(
                            cluster_name,
                            namespace,
                            gateway1_host,
                            gateway1_port,
                            gateway2_host,
                            gateway2_port,
                            source_instance_id,
                        )

                with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway1_forward:
                    with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT) as gateway2_forward:
                        gateway1_host, gateway1_port = wait_for_tcp_port_forward(gateway1_forward, timeout_seconds=30.0)
                        gateway2_host, gateway2_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                        run_server_restart_scenario(
                            cluster_name,
                            namespace,
                            gateway1_host,
                            gateway1_port,
                            gateway2_host,
                            gateway2_port,
                            source_instance_id,
                            source_world_id,
                        )
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run fault-injection continuity restart proofs on live kind clusters."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    try:
        run_stage(
            cluster_name=f"dynaxis-restart-{int(time.time())}",
            namespace="dynaxis-restart",
            timeout_seconds=args.wait_timeout_seconds,
        )
        print("PASS worlds-kubernetes-kind-restart")
        return 0
    except Exception as exc:
        print(f"FAIL worlds-kubernetes-kind-restart: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
