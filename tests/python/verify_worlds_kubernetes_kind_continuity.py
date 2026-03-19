from __future__ import annotations

import argparse
import hashlib
import socket
import sys
import tempfile
import time
from pathlib import Path

from session_continuity_common import ChatClient
from session_continuity_common import lp_utf8
from session_continuity_common import MSG_CHAT_SEND
from verify_worlds_kubernetes_kind import DEFAULT_TOPOLOGY
from verify_worlds_kubernetes_kind import RUNNER
from verify_worlds_kubernetes_kind import detect_prerequisites
from verify_worlds_kubernetes_kind import run
from verify_worlds_kubernetes_kind import skip
from verify_worlds_kubernetes_kind_closure import SAME_WORLD_TOPOLOGY
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_control_plane import load_json_http
from verify_worlds_kubernetes_kind_control_plane import request_json_http
from verify_worlds_kubernetes_kind_control_plane import wait_for_http_ok
from verify_worlds_kubernetes_kind_control_plane import wait_for_observed_topology
from verify_worlds_kubernetes_kind_control_plane import wait_for_port_forward
from verify_worlds_kubernetes_kind_control_plane import wait_for_json_ready


REPO_ROOT = Path(__file__).resolve().parents[2]
ADMIN_SERVICE_PORT = 39200
GATEWAY_SERVICE_PORT = 6000
SESSION_DIRECTORY_PREFIX = "gateway/session/"
CONTINUITY_WORLD_OWNER_PREFIX = "dynaxis:continuity:world-owner:"


def make_resume_routing_key(resume_token: str) -> str:
    digest = hashlib.sha256(resume_token.encode("utf-8")).hexdigest()
    return f"resume-hash:{digest}"


def assert_initial_login(login: dict, user: str) -> tuple[str, str]:
    if login.get("effective_user") != user:
        raise RuntimeError(f"effective user mismatch: {login}")
    if not login.get("logical_session_id") or not login.get("resume_token"):
        raise RuntimeError(f"continuity lease fields missing: {login}")
    if not login.get("world_id"):
        raise RuntimeError(f"world admission metadata missing: {login}")
    if login.get("resumed"):
        raise RuntimeError(f"initial login unexpectedly marked resumed: {login}")
    return str(login["logical_session_id"]), str(login["resume_token"])


def assert_resumed_login(login: dict, user: str, logical_session_id: str) -> None:
    if login.get("effective_user") != user:
        raise RuntimeError(f"resumed effective user mismatch: {login}")
    if login.get("logical_session_id") != logical_session_id:
        raise RuntimeError(f"logical session id changed across resume: {login}")
    if login.get("resumed") is not True:
        raise RuntimeError(f"resumed login not marked resumed: {login}")


def wait_for_tcp_port_forward(port_forward: PortForward, timeout_seconds: float) -> tuple[str, int]:
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


def redis_get(cluster_name: str, namespace: str, key: str) -> str:
    command = [
        "kubectl",
        "--context",
        f"kind-{cluster_name}",
        "--namespace",
        namespace,
        "exec",
        "redis-0",
        "--",
        "redis-cli",
        "--raw",
        "GET",
        key,
    ]
    result = run(command)
    return result.stdout.strip()


def wait_for_redis_value(cluster_name: str, namespace: str, key: str, timeout_seconds: float = 15.0) -> str:
    deadline = time.time() + timeout_seconds
    last_value = ""
    while time.time() < deadline:
        last_value = redis_get(cluster_name, namespace, key)
        if last_value:
            return last_value
        time.sleep(0.5)
    return last_value


def current_backend_for_user(cluster_name: str, namespace: str, user: str) -> str:
    return redis_get(cluster_name, namespace, f"{SESSION_DIRECTORY_PREFIX}{user}")


def current_backend_for_resume_token(cluster_name: str, namespace: str, resume_token: str) -> str:
    routing_key = make_resume_routing_key(resume_token)
    return redis_get(cluster_name, namespace, f"{SESSION_DIRECTORY_PREFIX}{routing_key}")


def connect_and_login(user: str, token: str, host: str, port: int) -> tuple[ChatClient, dict]:
    client = ChatClient(host=host, port=port)
    client.connect()
    login = client.login(user, token)
    return client, login


def acquire_session_for_backend(
    cluster_name: str,
    namespace: str,
    target_backend: str,
    host: str,
    port: int,
    prefix: str,
) -> tuple[ChatClient, dict, str]:
    observed_backends: list[str] = []
    for attempt in range(1, 25):
        user = f"{prefix}_{int(time.time())}_{attempt}"
        client, login = connect_and_login(user, "", host, port)
        backend = current_backend_for_user(cluster_name, namespace, user)
        if backend == target_backend:
            return client, login, user
        if backend:
            observed_backends.append(backend)
        client.close()
    raise RuntimeError(f"failed to land on backend {target_backend}; observed={observed_backends}")


def resume_until_success(
    cluster_name: str,
    namespace: str,
    host: str,
    port: int,
    resume_token: str,
    user: str,
    logical_session_id: str,
    timeout_seconds: float = 30.0,
) -> tuple[ChatClient, dict]:
    deadline = time.time() + timeout_seconds
    last_error: Exception | None = None
    while time.time() < deadline:
        client = ChatClient(host=host, port=port)
        try:
            client.connect()
            login = client.login("ignored_resume_user", "resume:" + resume_token)
            assert_resumed_login(login, user, logical_session_id)
            return client, login
        except Exception as exc:
            last_error = exc
            client.close()
            time.sleep(0.5)
    raise RuntimeError(f"resume timeout: {last_error}")


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
        raise RuntimeError("admin auth mode mismatch for kind continuity proof")
    if auth_data.get("read_only") is not False:
        raise RuntimeError("admin read_only should be false for kind continuity proof")
    capabilities = auth_data.get("capabilities", {})
    for capability in ("world_drain", "world_transfer", "world_migration"):
        if capabilities.get(capability) is not True:
            raise RuntimeError(f"admin capability is not writable: {capability}")


def select_default_worlds(observed_topology: dict) -> tuple[dict, dict]:
    worlds = observed_topology.get("data", {}).get("worlds", [])
    ready_worlds = [
        item
        for item in worlds
        if isinstance(item, dict)
        and item.get("world_id")
        and len(item.get("instances", [])) >= 1
    ]
    if len(ready_worlds) < 2:
        raise RuntimeError("default topology continuity proof requires at least two observed worlds")
    ready_worlds = sorted(ready_worlds, key=lambda item: str(item.get("world_id")))
    return ready_worlds[0], ready_worlds[1]


def wait_for_world_inventory(base_url: str, predicate, *, timeout_seconds: float) -> dict:
    return wait_for_world_endpoint(
        base_url,
        "/api/v1/worlds?limit=100",
        predicate,
        timeout_seconds=timeout_seconds,
    )


def select_same_world_transfer_source(worlds_payload: dict) -> tuple[dict, str, str]:
    worlds = worlds_payload.get("data", {}).get("items", [])
    for world in worlds:
        instances = world.get("instances", [])
        ready_instances = [
            item
            for item in instances
            if isinstance(item, dict) and item.get("instance_id") and item.get("ready") is True
        ]
        if len(ready_instances) >= 2:
            source_instance_id = str(ready_instances[0]["instance_id"])
            replacement_instance_id = str(ready_instances[1]["instance_id"])
            return world, source_instance_id, replacement_instance_id
    raise RuntimeError("same-world continuity proof requires a world with at least two ready server instances")


def run_default_migration_resume(
    cluster_name: str,
    namespace: str,
    base_url: str,
    gateway_host: str,
    gateway_port: int,
) -> None:
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
    room = f"kind_migration_room_{int(time.time())}"
    message = f"kind_migration_msg_{int(time.time() * 1000)}"

    first = None
    second = None
    drain_path = f"/api/v1/worlds/{source_world_id}/drain"
    migration_path = f"/api/v1/worlds/{source_world_id}/migration"
    try:
        first, login, user = acquire_session_for_backend(
            cluster_name,
            namespace,
            source_instance_id,
            gateway_host,
            gateway_port,
            "kind_migration",
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
            gateway_host,
            gateway_port,
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
            "PASS kind-default-migration-resume: "
            f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} world_id={target_world_id}"
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


def run_same_world_transfer_resume(
    cluster_name: str,
    namespace: str,
    base_url: str,
    gateway_host: str,
    gateway_port: int,
) -> None:
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
    room = f"kind_transfer_room_{int(time.time())}"
    message = f"kind_transfer_msg_{int(time.time() * 1000)}"
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
            gateway_host,
            gateway_port,
            "kind_transfer",
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
            gateway_host,
            gateway_port,
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
            "PASS kind-same-world-transfer-resume: "
            f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} world_id={source_world_id}"
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
    topology_config: Path,
    scenario_name: str,
    timeout_seconds: int,
) -> None:
    with tempfile.TemporaryDirectory(prefix=f"dynaxis-kind-continuity-{scenario_name}-") as temp_dir_raw:
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
                with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway_forward:
                    base_url = wait_for_port_forward(admin_forward, timeout_seconds=30.0)
                    gateway_host, gateway_port = wait_for_tcp_port_forward(gateway_forward, timeout_seconds=30.0)
                    wait_for_http_ok(base_url, "/readyz", timeout_seconds=15.0)
                    assert_world_capabilities(base_url)
                    wait_for_observed_topology(base_url, timeout_seconds=30.0)

                    if scenario_name == "default-migration-resume":
                        run_default_migration_resume(
                            cluster_name,
                            namespace,
                            base_url,
                            gateway_host,
                            gateway_port,
                        )
                    elif scenario_name == "same-world-transfer-resume":
                        run_same_world_transfer_resume(
                            cluster_name,
                            namespace,
                            base_url,
                            gateway_host,
                            gateway_port,
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
        description="Run topology-aware client continuity/resume proofs on live kind clusters."
    )
    parser.add_argument(
        "--scenario",
        choices=("matrix", "default-migration-resume", "same-world-transfer-resume"),
        default="matrix",
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    try:
        if args.scenario in {"matrix", "default-migration-resume"}:
            run_stage(
                cluster_name=f"dynaxis-continuity-default-{int(time.time())}",
                namespace="dynaxis-continuity-default",
                topology_config=DEFAULT_TOPOLOGY,
                scenario_name="default-migration-resume",
                timeout_seconds=args.wait_timeout_seconds,
            )
        if args.scenario in {"matrix", "same-world-transfer-resume"}:
            run_stage(
                cluster_name=f"dynaxis-continuity-sameworld-{int(time.time())}",
                namespace="dynaxis-continuity-sameworld",
                topology_config=SAME_WORLD_TOPOLOGY,
                scenario_name="same-world-transfer-resume",
                timeout_seconds=args.wait_timeout_seconds,
            )
        print("PASS worlds-kubernetes-kind-continuity")
        return 0
    except Exception as exc:
        print(f"FAIL worlds-kubernetes-kind-continuity: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
