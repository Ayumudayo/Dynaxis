from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from contextlib import ExitStack
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
from verify_worlds_kubernetes_kind_continuity import (
    ADMIN_SERVICE_PORT,
    GATEWAY_SERVICE_PORT,
    CONTINUITY_WORLD_OWNER_PREFIX,
    SESSION_DIRECTORY_PREFIX,
    acquire_session_for_backend,
    assert_initial_login,
    current_backend_for_resume_token,
    delete_world_endpoint,
    make_resume_routing_key,
    redis_get,
    resume_until_success,
    select_default_worlds,
    select_same_world_transfer_source,
    wait_for_redis_value,
    wait_for_tcp_port_forward,
    wait_for_world_endpoint,
    wait_for_world_inventory,
)
from verify_worlds_kubernetes_kind_control_plane import PortForward
from verify_worlds_kubernetes_kind_control_plane import request_json_http
from verify_worlds_kubernetes_kind_control_plane import wait_for_http_ok
from verify_worlds_kubernetes_kind_control_plane import wait_for_observed_topology
from verify_worlds_kubernetes_kind_control_plane import wait_for_port_forward
from verify_worlds_kubernetes_kind_metrics_budget import (
    GATEWAY_METRICS_PORT,
    SERVER_METRICS_PORT,
    assert_after_at_least,
    assert_delta_positive,
    assert_delta_zero,
    capture_metrics,
    gateway_login_metric_specs,
    gateway_resume_metric_specs,
    metric_series,
    migration_server_metric_specs,
    transfer_server_metric_specs,
    wait_for_metrics_endpoint,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
RESUME_LOCATOR_PREFIX = SESSION_DIRECTORY_PREFIX + "locator/"


def locator_key_for_resume_token(resume_token: str) -> str:
    return f"{RESUME_LOCATOR_PREFIX}{make_resume_routing_key(resume_token)}"


def redis_delete(cluster_name: str, namespace: str, key: str) -> None:
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
        "DEL",
        key,
    ]
    run(command)


def assert_locator_hint_present(cluster_name: str, namespace: str, resume_token: str) -> str:
    locator_key = locator_key_for_resume_token(resume_token)
    locator_payload = wait_for_redis_value(cluster_name, namespace, locator_key)
    if not locator_payload:
        raise RuntimeError(f"resume locator hint missing: {locator_key}")
    return locator_payload


def write_locator_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "locator-fallback.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"locator fallback report: {report_path}")


def run_default_migration_locator_fallback(
    cluster_name: str,
    namespace: str,
    base_url: str,
    login_host: str,
    login_port: int,
    resume_host: str,
    resume_port: int,
) -> dict[str, object]:
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
    room = f"kind_locator_migration_room_{int(time.time())}"
    message = f"kind_locator_migration_msg_{int(time.time() * 1000)}"
    drain_path = f"/api/v1/worlds/{source_world_id}/drain"
    migration_path = f"/api/v1/worlds/{source_world_id}/migration"

    first: ChatClient | None = None
    second: ChatClient | None = None
    try:
        with ExitStack() as stack:
            login_gateway_metrics_forward = stack.enter_context(
                PortForward(cluster_name, namespace, "gateway-1", GATEWAY_METRICS_PORT)
            )
            resume_gateway_metrics_forward = stack.enter_context(
                PortForward(cluster_name, namespace, "gateway-2", GATEWAY_METRICS_PORT)
            )
            target_server_metrics_forward = stack.enter_context(
                PortForward(
                    cluster_name,
                    namespace,
                    target_owner_instance_id,
                    SERVER_METRICS_PORT,
                    resource_kind="pod",
                )
            )

            login_gateway_metrics_url = wait_for_metrics_endpoint(login_gateway_metrics_forward, timeout_seconds=20.0)
            resume_gateway_metrics_url = wait_for_metrics_endpoint(resume_gateway_metrics_forward, timeout_seconds=20.0)
            target_server_metrics_url = wait_for_metrics_endpoint(target_server_metrics_forward, timeout_seconds=20.0)

            login_gateway_before = capture_metrics(login_gateway_metrics_url, gateway_login_metric_specs())
            resume_gateway_before = capture_metrics(resume_gateway_metrics_url, gateway_resume_metric_specs())
            target_server_before = capture_metrics(target_server_metrics_url, migration_server_metric_specs())

            first, login, user = acquire_session_for_backend(
                cluster_name,
                namespace,
                source_instance_id,
                login_host,
                login_port,
                "kind_locator_migration",
            )
            logical_session_id, resume_token = assert_initial_login(login, user)
            if login.get("world_id") != source_world_id:
                raise RuntimeError(f"unexpected initial world residency: {login}")
            first.join_room(room, user)

            alias_key = f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}"
            alias_backend_before = wait_for_redis_value(cluster_name, namespace, alias_key)
            if alias_backend_before != source_instance_id:
                raise RuntimeError(f"resume alias was not attached to {source_instance_id} before migration proof")
            locator_payload = assert_locator_hint_present(cluster_name, namespace, resume_token)

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
                body={"replacement_owner_instance_id": None},
            )
            if status != 200:
                raise RuntimeError(f"world drain PUT failed: status={status} payload={payload}")

            redis_delete(cluster_name, namespace, alias_key)
            if redis_get(cluster_name, namespace, alias_key):
                raise RuntimeError("exact resume alias binding still exists after delete")
            first.close()
            first = None

            wait_for_world_endpoint(
                base_url,
                migration_path,
                lambda candidate: (candidate.get("data", {}).get("migration", {}) or {}).get("phase")
                == "ready_to_resume",
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

            login_gateway_after = capture_metrics(login_gateway_metrics_url, gateway_login_metric_specs())
            resume_gateway_after = capture_metrics(resume_gateway_metrics_url, gateway_resume_metric_specs())
            target_server_after = capture_metrics(target_server_metrics_url, migration_server_metric_specs())

            login_gateway_series = metric_series(login_gateway_before, login_gateway_after)
            resume_gateway_series = metric_series(resume_gateway_before, resume_gateway_after)
            target_server_series = metric_series(target_server_before, target_server_after)

            assert_delta_positive(login_gateway_series, "resume_routing_bind_total")
            assert_delta_positive(login_gateway_series, "resume_locator_bind_total")

            assert_after_at_least(resume_gateway_series, "sessions_active", 1.0)
            assert_delta_zero(resume_gateway_series, "resume_routing_hit_total")
            assert_delta_positive(resume_gateway_series, "resume_locator_lookup_hit_total")
            assert_delta_zero(resume_gateway_series, "resume_locator_lookup_miss_total")
            assert_delta_zero(resume_gateway_series, "resume_locator_selector_hit_total")
            assert_delta_positive(resume_gateway_series, "resume_locator_selector_fallback_total")
            assert_delta_zero(resume_gateway_series, "world_policy_filtered_sticky_total")
            assert_delta_positive(resume_gateway_series, "world_policy_filtered_candidate_total")
            assert_delta_zero(resume_gateway_series, "world_policy_replacement_selected_total")

            assert_after_at_least(target_server_series, "chat_session_active", 1.0)
            assert_delta_positive(target_server_series, "lease_resume_total")
            assert_delta_zero(target_server_series, "lease_resume_fail_total")
            assert_delta_positive(target_server_series, "state_restore_total")
            assert_delta_zero(target_server_series, "state_restore_fallback_total")
            assert_delta_zero(target_server_series, "world_restore_total")
            assert_delta_zero(target_server_series, "world_restore_fallback_total")
            assert_delta_zero(target_server_series, "world_owner_restore_total")
            assert_delta_positive(target_server_series, "world_migration_restore_total")
            assert_delta_zero(target_server_series, "world_migration_restore_fallback_total")

            alias_backend_after = wait_for_redis_value(cluster_name, namespace, alias_key)
            if alias_backend_after != target_owner_instance_id:
                raise RuntimeError(
                    f"resume alias was not rewritten to the migration target backend: {alias_backend_after!r}"
                )

            report = {
                "scenario": "default-migration-locator-fallback",
                "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                "login_gateway": "gateway-1",
                "resume_gateway": "gateway-2",
                "source_world_id": source_world_id,
                "target_world_id": target_world_id,
                "source_instance_id": source_instance_id,
                "target_instance_id": target_owner_instance_id,
                "logical_session_id": logical_session_id,
                "locator_payload": locator_payload,
                "resumed_backend": resumed_backend,
                "metrics": {
                    "gateway-1": login_gateway_series,
                    "gateway-2": resume_gateway_series,
                    target_owner_instance_id: target_server_series,
                },
            }

            delete_world_endpoint(base_url, drain_path)
            delete_world_endpoint(base_url, migration_path)

            print(
                "PASS kind-default-migration-locator-fallback: "
                f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} "
                f"selector_fallback_delta={resume_gateway_series['resume_locator_selector_fallback_total']['delta']:.0f}"
            )
            return report
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


def run_same_world_transfer_locator_fallback(
    cluster_name: str,
    namespace: str,
    base_url: str,
    login_host: str,
    login_port: int,
    resume_host: str,
    resume_port: int,
) -> dict[str, object]:
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
    room = f"kind_locator_transfer_room_{int(time.time())}"
    message = f"kind_locator_transfer_msg_{int(time.time() * 1000)}"
    world_owner_key = f"{CONTINUITY_WORLD_OWNER_PREFIX}{source_world_id}"
    drain_path = f"/api/v1/worlds/{source_world_id}/drain"
    transfer_path = f"/api/v1/worlds/{source_world_id}/transfer"

    first: ChatClient | None = None
    second: ChatClient | None = None
    try:
        with ExitStack() as stack:
            login_gateway_metrics_forward = stack.enter_context(
                PortForward(cluster_name, namespace, "gateway-1", GATEWAY_METRICS_PORT)
            )
            resume_gateway_metrics_forward = stack.enter_context(
                PortForward(cluster_name, namespace, "gateway-2", GATEWAY_METRICS_PORT)
            )
            target_server_metrics_forward = stack.enter_context(
                PortForward(
                    cluster_name,
                    namespace,
                    replacement_owner_instance_id,
                    SERVER_METRICS_PORT,
                    resource_kind="pod",
                )
            )

            login_gateway_metrics_url = wait_for_metrics_endpoint(login_gateway_metrics_forward, timeout_seconds=20.0)
            resume_gateway_metrics_url = wait_for_metrics_endpoint(resume_gateway_metrics_forward, timeout_seconds=20.0)
            target_server_metrics_url = wait_for_metrics_endpoint(target_server_metrics_forward, timeout_seconds=20.0)

            login_gateway_before = capture_metrics(login_gateway_metrics_url, gateway_login_metric_specs())
            resume_gateway_before = capture_metrics(resume_gateway_metrics_url, gateway_resume_metric_specs())
            target_server_before = capture_metrics(target_server_metrics_url, transfer_server_metric_specs())

            first, login, user = acquire_session_for_backend(
                cluster_name,
                namespace,
                source_instance_id,
                login_host,
                login_port,
                "kind_locator_transfer",
            )
            logical_session_id, resume_token = assert_initial_login(login, user)
            if login.get("world_id") != source_world_id:
                raise RuntimeError(f"unexpected initial world residency: {login}")
            first.join_room(room, user)

            alias_key = f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}"
            alias_backend_before = wait_for_redis_value(cluster_name, namespace, alias_key)
            if alias_backend_before != source_instance_id:
                raise RuntimeError(f"resume alias was not attached to {source_instance_id} before transfer proof")
            locator_payload = assert_locator_hint_present(cluster_name, namespace, resume_token)
            if wait_for_redis_value(cluster_name, namespace, world_owner_key) != source_instance_id:
                raise RuntimeError(f"world owner key was not attached to {source_instance_id} before transfer proof")

            status, payload = request_json_http(
                base_url,
                drain_path,
                method="PUT",
                body={"replacement_owner_instance_id": replacement_owner_instance_id},
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

            redis_delete(cluster_name, namespace, alias_key)
            if redis_get(cluster_name, namespace, alias_key):
                raise RuntimeError("exact resume alias binding still exists after delete")
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

            login_gateway_after = capture_metrics(login_gateway_metrics_url, gateway_login_metric_specs())
            resume_gateway_after = capture_metrics(resume_gateway_metrics_url, gateway_resume_metric_specs())
            target_server_after = capture_metrics(target_server_metrics_url, transfer_server_metric_specs())

            login_gateway_series = metric_series(login_gateway_before, login_gateway_after)
            resume_gateway_series = metric_series(resume_gateway_before, resume_gateway_after)
            target_server_series = metric_series(target_server_before, target_server_after)

            assert_delta_positive(login_gateway_series, "resume_routing_bind_total")
            assert_delta_positive(login_gateway_series, "resume_locator_bind_total")

            assert_after_at_least(resume_gateway_series, "sessions_active", 1.0)
            assert_delta_zero(resume_gateway_series, "resume_routing_hit_total")
            assert_delta_positive(resume_gateway_series, "resume_locator_lookup_hit_total")
            assert_delta_zero(resume_gateway_series, "resume_locator_lookup_miss_total")
            assert_delta_positive(resume_gateway_series, "resume_locator_selector_hit_total")
            assert_delta_zero(resume_gateway_series, "resume_locator_selector_fallback_total")
            assert_delta_zero(resume_gateway_series, "world_policy_filtered_sticky_total")
            assert_delta_positive(resume_gateway_series, "world_policy_filtered_candidate_total")
            assert_delta_positive(resume_gateway_series, "world_policy_replacement_selected_total")

            assert_after_at_least(target_server_series, "chat_session_active", 1.0)
            assert_delta_positive(target_server_series, "lease_resume_total")
            assert_delta_zero(target_server_series, "lease_resume_fail_total")
            assert_delta_positive(target_server_series, "state_restore_total")
            assert_delta_zero(target_server_series, "state_restore_fallback_total")
            assert_delta_positive(target_server_series, "world_restore_total")
            assert_delta_zero(target_server_series, "world_restore_fallback_total")
            assert_delta_positive(target_server_series, "world_owner_restore_total")
            assert_delta_zero(target_server_series, "world_owner_restore_fallback_total")
            assert_delta_zero(target_server_series, "world_migration_restore_total")

            alias_backend_after = wait_for_redis_value(cluster_name, namespace, alias_key)
            if alias_backend_after != replacement_owner_instance_id:
                raise RuntimeError(
                    f"resume alias was not rewritten to the committed replacement backend: {alias_backend_after!r}"
                )

            report = {
                "scenario": "same-world-transfer-locator-fallback",
                "topology_config": str(SAME_WORLD_TOPOLOGY.relative_to(REPO_ROOT)),
                "login_gateway": "gateway-1",
                "resume_gateway": "gateway-2",
                "world_id": source_world_id,
                "source_instance_id": source_instance_id,
                "target_instance_id": replacement_owner_instance_id,
                "logical_session_id": logical_session_id,
                "locator_payload": locator_payload,
                "resumed_backend": resumed_backend,
                "metrics": {
                    "gateway-1": login_gateway_series,
                    "gateway-2": resume_gateway_series,
                    replacement_owner_instance_id: target_server_series,
                },
            }

            delete_world_endpoint(base_url, drain_path)
            delete_world_endpoint(base_url, transfer_path)

            print(
                "PASS kind-same-world-transfer-locator-fallback: "
                f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} "
                f"selector_hit_delta={resume_gateway_series['resume_locator_selector_hit_total']['delta']:.0f}"
            )
            return report
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
) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix=f"dynaxis-kind-locator-{scenario_name}-") as temp_dir_raw:
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
                with PortForward(cluster_name, namespace, "gateway-1", GATEWAY_SERVICE_PORT) as gateway1_forward:
                    with PortForward(cluster_name, namespace, "gateway-2", GATEWAY_SERVICE_PORT) as gateway2_forward:
                        base_url = wait_for_port_forward(admin_forward, timeout_seconds=30.0)
                        login_host, login_port = wait_for_tcp_port_forward(gateway1_forward, timeout_seconds=30.0)
                        resume_host, resume_port = wait_for_tcp_port_forward(gateway2_forward, timeout_seconds=30.0)
                        wait_for_http_ok(base_url, "/readyz", timeout_seconds=15.0)
                        wait_for_observed_topology(base_url, timeout_seconds=30.0)

                        if scenario_name == "default-migration-locator-fallback":
                            return run_default_migration_locator_fallback(
                                cluster_name,
                                namespace,
                                base_url,
                                login_host,
                                login_port,
                                resume_host,
                                resume_port,
                            )
                        if scenario_name == "same-world-transfer-locator-fallback":
                            return run_same_world_transfer_locator_fallback(
                                cluster_name,
                                namespace,
                                base_url,
                                login_host,
                                login_port,
                                resume_host,
                                resume_port,
                            )
                        raise RuntimeError(f"unknown scenario: {scenario_name}")
        finally:
            cleanup = run(down_command, check=False)
            if cleanup.returncode != 0:
                print(cleanup.stdout, end="")
                print(cleanup.stderr, end="")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run live kind locator-fallback proofs for migration and transfer continuity flows."
    )
    parser.add_argument(
        "--scenario",
        choices=("matrix", "default-migration-locator-fallback", "same-world-transfer-locator-fallback"),
        default="matrix",
    )
    parser.add_argument("--wait-timeout-seconds", type=int, default=240)
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    prerequisite_issue = detect_prerequisites()
    if prerequisite_issue is not None:
        return skip(prerequisite_issue)

    reports: list[dict[str, object]] = []
    try:
        if args.scenario in {"matrix", "default-migration-locator-fallback"}:
            reports.append(
                run_stage(
                    cluster_name=f"dynaxis-locator-default-{int(time.time())}",
                    namespace="dynaxis-locator-default",
                    topology_config=DEFAULT_TOPOLOGY,
                    scenario_name="default-migration-locator-fallback",
                    timeout_seconds=args.wait_timeout_seconds,
                )
            )
        if args.scenario in {"matrix", "same-world-transfer-locator-fallback"}:
            reports.append(
                run_stage(
                    cluster_name=f"dynaxis-locator-sameworld-{int(time.time())}",
                    namespace="dynaxis-locator-sameworld",
                    topology_config=SAME_WORLD_TOPOLOGY,
                    scenario_name="same-world-transfer-locator-fallback",
                    timeout_seconds=args.wait_timeout_seconds,
                )
            )
        report = {
            "proof": "worlds-kubernetes-kind-locator-fallback",
            "scenario": args.scenario,
            "scenarios": reports,
        }
        write_locator_report(report, args.artifact_dir)
        print("PASS worlds-kubernetes-kind-locator-fallback")
        return 0
    except Exception as exc:
        print(f"FAIL worlds-kubernetes-kind-locator-fallback: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
