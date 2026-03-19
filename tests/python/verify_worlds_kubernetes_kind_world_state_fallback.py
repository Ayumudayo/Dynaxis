from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path

from session_continuity_common import ChatClient
from session_continuity_common import MSG_CHAT_SEND
from session_continuity_common import lp_utf8
from verify_worlds_kubernetes_kind import DEFAULT_TOPOLOGY
from verify_worlds_kubernetes_kind import RUNNER
from verify_worlds_kubernetes_kind import detect_prerequisites
from verify_worlds_kubernetes_kind import run
from verify_worlds_kubernetes_kind import skip
from verify_worlds_kubernetes_kind_continuity import (
    ADMIN_SERVICE_PORT,
    GATEWAY_SERVICE_PORT,
    CONTINUITY_WORLD_OWNER_PREFIX,
    SESSION_DIRECTORY_PREFIX,
    acquire_session_for_backend,
    assert_initial_login,
    current_backend_for_resume_token,
    make_resume_routing_key,
    redis_get,
    resume_until_success,
    select_default_worlds,
    wait_for_tcp_port_forward,
)
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_control_plane import wait_for_http_ok
from verify_worlds_kubernetes_kind_control_plane import wait_for_observed_topology
from verify_worlds_kubernetes_kind_control_plane import wait_for_port_forward
from verify_worlds_kubernetes_kind_locator_fallback import redis_delete
from verify_worlds_kubernetes_kind_metrics_budget import (
    GATEWAY_METRICS_PORT,
    SERVER_METRICS_PORT,
    assert_after_at_least,
    assert_delta_positive,
    assert_delta_zero,
    capture_metrics,
    metric_series,
    wait_for_metrics_endpoint,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTINUITY_WORLD_PREFIX = "dynaxis:continuity:world:"
ROOM_MISMATCH_ERRC = 0x0106


def write_world_state_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "world-state-fallback.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"world-state fallback report: {report_path}")


def gateway_resume_hit_specs() -> list[tuple[str, str, dict[str, str] | None]]:
    return [
        ("sessions_active", "gateway_sessions_active", None),
        ("resume_routing_hit_total", "gateway_resume_routing_hit_total", None),
        ("resume_locator_lookup_hit_total", "gateway_resume_locator_lookup_hit_total", None),
        ("resume_locator_lookup_miss_total", "gateway_resume_locator_lookup_miss_total", None),
        ("resume_locator_selector_hit_total", "gateway_resume_locator_selector_hit_total", None),
        ("resume_locator_selector_fallback_total", "gateway_resume_locator_selector_fallback_total", None),
    ]


def world_state_server_specs() -> list[tuple[str, str, dict[str, str] | None]]:
    return [
        ("chat_session_active", "chat_session_active", None),
        ("lease_resume_total", "chat_continuity_lease_resume_total", None),
        ("lease_resume_fail_total", "chat_continuity_lease_resume_fail_total", None),
        ("world_restore_fallback_total", "chat_continuity_world_restore_fallback_total", None),
        ("world_owner_restore_fallback_total", "chat_continuity_world_owner_restore_fallback_total", None),
        (
            "world_restore_missing_world_total",
            "chat_continuity_world_restore_fallback_reason_total",
            {"reason": "missing_world"},
        ),
        (
            "world_restore_missing_owner_total",
            "chat_continuity_world_restore_fallback_reason_total",
            {"reason": "missing_owner"},
        ),
    ]


def assert_room_reset(client: ChatClient, room: str, suffix: str) -> None:
    client.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(suffix))
    error_code, error_message = client.wait_for_error(5.0)
    if error_code != ROOM_MISMATCH_ERRC or error_message != "room mismatch":
        raise RuntimeError(
            f"room reset was not enforced after fallback: code={error_code} message={error_message!r}"
        )


def run_world_residency_fallback(
    cluster_name: str,
    namespace: str,
    gateway_host: str,
    gateway_port: int,
    source_world_id: str,
    source_instance_id: str,
    resume_gateway_metrics_url: str,
    source_server_metrics_url: str,
) -> dict[str, object]:
    room = f"kind_world_fallback_room_{int(time.time())}"
    first: ChatClient | None = None
    second: ChatClient | None = None
    try:
        gateway_before = capture_metrics(resume_gateway_metrics_url, gateway_resume_hit_specs())
        server_before = capture_metrics(source_server_metrics_url, world_state_server_specs())

        first, login, user = acquire_session_for_backend(
            cluster_name,
            namespace,
            source_instance_id,
            gateway_host,
            gateway_port,
            "kind_world_fallback",
        )
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login.get("world_id") != source_world_id:
            raise RuntimeError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        alias_key = f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}"
        alias_backend_before = redis_get(cluster_name, namespace, alias_key)
        if alias_backend_before != source_instance_id:
            raise RuntimeError(f"resume alias was not attached to {source_instance_id}: {alias_backend_before!r}")

        world_key = f"{CONTINUITY_WORLD_PREFIX}{logical_session_id}"
        if redis_get(cluster_name, namespace, world_key) != source_world_id:
            raise RuntimeError(f"world residency key did not persist {source_world_id} before fallback proof")

        redis_delete(cluster_name, namespace, world_key)
        if redis_get(cluster_name, namespace, world_key):
            raise RuntimeError("world residency key still exists after delete")
        first.close()
        first = None

        second, resumed = resume_until_success(
            cluster_name,
            namespace,
            gateway_host,
            gateway_port,
            resume_token,
            user,
            logical_session_id,
        )
        if resumed.get("world_id") != source_world_id:
            raise RuntimeError(f"world residency fallback did not land on safe default world: {resumed}")
        if current_backend_for_resume_token(cluster_name, namespace, resume_token) != source_instance_id:
            raise RuntimeError("resume alias was not preserved on the original backend after world fallback")

        assert_room_reset(second, room, "should_fail_after_world_fallback")

        gateway_after = capture_metrics(resume_gateway_metrics_url, gateway_resume_hit_specs())
        server_after = capture_metrics(source_server_metrics_url, world_state_server_specs())
        gateway_series = metric_series(gateway_before, gateway_after)
        server_series = metric_series(server_before, server_after)

        assert_after_at_least(gateway_series, "sessions_active", 1.0)
        assert_delta_positive(gateway_series, "resume_routing_hit_total")
        assert_delta_zero(gateway_series, "resume_locator_lookup_hit_total")
        assert_delta_zero(gateway_series, "resume_locator_lookup_miss_total")
        assert_delta_zero(gateway_series, "resume_locator_selector_hit_total")
        assert_delta_zero(gateway_series, "resume_locator_selector_fallback_total")

        assert_after_at_least(server_series, "chat_session_active", 1.0)
        assert_delta_positive(server_series, "lease_resume_total")
        assert_delta_zero(server_series, "lease_resume_fail_total")
        assert_delta_positive(server_series, "world_restore_fallback_total")
        assert_delta_zero(server_series, "world_owner_restore_fallback_total")
        assert_delta_positive(server_series, "world_restore_missing_world_total")
        assert_delta_zero(server_series, "world_restore_missing_owner_total")

        report = {
            "scenario": "world-residency-fallback",
            "world_id": source_world_id,
            "instance_id": source_instance_id,
            "logical_session_id": logical_session_id,
            "metrics": {
                "gateway-2": gateway_series,
                source_instance_id: server_series,
            },
        }
        print(
            "PASS kind-world-residency-fallback: "
            f"logical_session_id={logical_session_id} fallback_delta={server_series['world_restore_fallback_total']['delta']:.0f}"
        )
        return report
    finally:
        if first is not None:
            first.close()
        if second is not None:
            second.close()


def run_world_owner_missing_fallback(
    cluster_name: str,
    namespace: str,
    gateway_host: str,
    gateway_port: int,
    source_world_id: str,
    source_instance_id: str,
    resume_gateway_metrics_url: str,
    source_server_metrics_url: str,
) -> dict[str, object]:
    room = f"kind_world_owner_missing_room_{int(time.time())}"
    first: ChatClient | None = None
    second: ChatClient | None = None
    try:
        gateway_before = capture_metrics(resume_gateway_metrics_url, gateway_resume_hit_specs())
        server_before = capture_metrics(source_server_metrics_url, world_state_server_specs())

        first, login, user = acquire_session_for_backend(
            cluster_name,
            namespace,
            source_instance_id,
            gateway_host,
            gateway_port,
            "kind_world_owner_missing",
        )
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login.get("world_id") != source_world_id:
            raise RuntimeError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        alias_key = f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}"
        alias_backend_before = redis_get(cluster_name, namespace, alias_key)
        if alias_backend_before != source_instance_id:
            raise RuntimeError(f"resume alias was not attached to {source_instance_id}: {alias_backend_before!r}")

        world_owner_key = f"{CONTINUITY_WORLD_OWNER_PREFIX}{source_world_id}"
        if redis_get(cluster_name, namespace, world_owner_key) != source_instance_id:
            raise RuntimeError(f"world owner key did not persist {source_instance_id} before fallback proof")

        redis_delete(cluster_name, namespace, world_owner_key)
        if redis_get(cluster_name, namespace, world_owner_key):
            raise RuntimeError("world owner key still exists after delete")
        first.close()
        first = None

        second, resumed = resume_until_success(
            cluster_name,
            namespace,
            gateway_host,
            gateway_port,
            resume_token,
            user,
            logical_session_id,
        )
        if resumed.get("world_id") != source_world_id:
            raise RuntimeError(f"world owner missing fallback did not land on safe default world: {resumed}")
        if current_backend_for_resume_token(cluster_name, namespace, resume_token) != source_instance_id:
            raise RuntimeError("resume alias was not preserved on the original backend after owner fallback")

        assert_room_reset(second, room, "should_fail_after_world_owner_missing_fallback")

        gateway_after = capture_metrics(resume_gateway_metrics_url, gateway_resume_hit_specs())
        server_after = capture_metrics(source_server_metrics_url, world_state_server_specs())
        gateway_series = metric_series(gateway_before, gateway_after)
        server_series = metric_series(server_before, server_after)

        assert_after_at_least(gateway_series, "sessions_active", 1.0)
        assert_delta_positive(gateway_series, "resume_routing_hit_total")
        assert_delta_zero(gateway_series, "resume_locator_lookup_hit_total")
        assert_delta_zero(gateway_series, "resume_locator_lookup_miss_total")
        assert_delta_zero(gateway_series, "resume_locator_selector_hit_total")
        assert_delta_zero(gateway_series, "resume_locator_selector_fallback_total")

        assert_after_at_least(server_series, "chat_session_active", 1.0)
        assert_delta_positive(server_series, "lease_resume_total")
        assert_delta_zero(server_series, "lease_resume_fail_total")
        assert_delta_positive(server_series, "world_restore_fallback_total")
        assert_delta_positive(server_series, "world_owner_restore_fallback_total")
        assert_delta_zero(server_series, "world_restore_missing_world_total")
        assert_delta_positive(server_series, "world_restore_missing_owner_total")

        if redis_get(cluster_name, namespace, world_owner_key) != source_instance_id:
            raise RuntimeError("world owner key was not rewritten to the current backend owner after fallback")

        report = {
            "scenario": "world-owner-missing-fallback",
            "world_id": source_world_id,
            "instance_id": source_instance_id,
            "logical_session_id": logical_session_id,
            "metrics": {
                "gateway-2": gateway_series,
                source_instance_id: server_series,
            },
        }
        print(
            "PASS kind-world-owner-missing-fallback: "
            f"logical_session_id={logical_session_id} owner_reason_delta={server_series['world_restore_missing_owner_total']['delta']:.0f}"
        )
        return report
    finally:
        if first is not None:
            first.close()
        if second is not None:
            second.close()


def run_stage(*, cluster_name: str, namespace: str, timeout_seconds: int) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix="dynaxis-kind-world-state-fallback-") as temp_dir_raw:
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
            with ExitStack() as stack:
                admin_forward = stack.enter_context(PortForward(cluster_name, namespace, "admin-app", ADMIN_SERVICE_PORT))
                gateway2_forward = stack.enter_context(PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT))
                gateway2_metrics_forward = stack.enter_context(PortForward(cluster_name, namespace, "gateway-2", GATEWAY_METRICS_PORT))

                base_url = wait_for_port_forward(admin_forward, timeout_seconds=30.0)
                gateway_host, gateway_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                gateway_metrics_url = wait_for_metrics_endpoint(gateway2_metrics_forward, timeout_seconds=20.0)
                wait_for_http_ok(base_url, "/readyz", timeout_seconds=15.0)
                observed_topology = wait_for_observed_topology(base_url, timeout_seconds=30.0)
                source_world, _target_world = select_default_worlds(observed_topology)
                source_world_id = str(source_world["world_id"])
                source_instance_id = str(source_world["instances"][0]["instance_id"])

                source_server_metrics_forward = stack.enter_context(
                    PortForward(
                        cluster_name,
                        namespace,
                        source_instance_id,
                        SERVER_METRICS_PORT,
                        resource_kind="pod",
                    )
                )
                source_server_metrics_url = wait_for_metrics_endpoint(source_server_metrics_forward, timeout_seconds=20.0)

                scenarios = [
                    run_world_residency_fallback(
                        cluster_name,
                        namespace,
                        gateway_host,
                        gateway_port,
                        source_world_id,
                        source_instance_id,
                        gateway_metrics_url,
                        source_server_metrics_url,
                    ),
                    run_world_owner_missing_fallback(
                        cluster_name,
                        namespace,
                        gateway_host,
                        gateway_port,
                        source_world_id,
                        source_instance_id,
                        gateway_metrics_url,
                        source_server_metrics_url,
                    ),
                ]
                return {
                    "proof": "worlds-kubernetes-kind-world-state-fallback",
                    "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                    "world_id": source_world_id,
                    "instance_id": source_instance_id,
                    "scenarios": scenarios,
                }
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run live kind server-side world/world-owner fallback proofs."
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    try:
        report = run_stage(
            cluster_name=f"dynaxis-world-fallback-{int(time.time())}",
            namespace="dynaxis-world-fallback",
            timeout_seconds=args.wait_timeout_seconds,
        )
        write_world_state_report(report, args.artifact_dir)
        print("PASS worlds-kubernetes-kind-world-state-fallback")
        return 0
    except Exception as exc:
        print(f"FAIL worlds-kubernetes-kind-world-state-fallback: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
