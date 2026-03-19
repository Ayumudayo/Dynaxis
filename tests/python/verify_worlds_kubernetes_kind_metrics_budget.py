from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
import urllib.request
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


REPO_ROOT = Path(__file__).resolve().parents[2]
GATEWAY_METRICS_PORT = 6001
SERVER_METRICS_PORT = 9090


def fetch_metrics_text(base_url: str) -> str:
    with urllib.request.urlopen(f"{base_url}/metrics", timeout=5.0) as response:
        return response.read().decode("utf-8", errors="replace")


def parse_labels(label_blob: str) -> dict[str, str]:
    labels: dict[str, str] = {}
    if not label_blob:
        return labels
    for entry in label_blob.split(","):
        if "=" not in entry:
            continue
        key, raw_value = entry.split("=", 1)
        labels[key] = raw_value.strip().strip('"')
    return labels


def parse_metric_value(metrics_text: str, metric_name: str, labels: dict[str, str] | None = None) -> float:
    expected_labels = labels or {}
    for line in metrics_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        try:
            metric_key, raw_value = stripped.rsplit(" ", 1)
        except ValueError:
            continue
        if "{" in metric_key and metric_key.endswith("}"):
            name, label_blob = metric_key.split("{", 1)
            parsed_labels = parse_labels(label_blob[:-1])
        else:
            name = metric_key
            parsed_labels = {}
        if name != metric_name:
            continue
        if parsed_labels != expected_labels:
            continue
        return float(raw_value)
    raise RuntimeError(f"metric not found: {metric_name} labels={expected_labels}")


def capture_metrics(
    base_url: str,
    specs: list[tuple[str, str, dict[str, str] | None]],
) -> dict[str, float]:
    metrics_text = fetch_metrics_text(base_url)
    return {
        metric_id: parse_metric_value(metrics_text, metric_name, labels)
        for metric_id, metric_name, labels in specs
    }


def metric_series(before: dict[str, float], after: dict[str, float]) -> dict[str, dict[str, float]]:
    out: dict[str, dict[str, float]] = {}
    for key, before_value in before.items():
        after_value = after[key]
        out[key] = {
            "before": before_value,
            "after": after_value,
            "delta": round(after_value - before_value, 3),
        }
    return out


def assert_delta_positive(series: dict[str, dict[str, float]], metric_key: str) -> None:
    if series[metric_key]["delta"] <= 0.0:
        raise RuntimeError(f"{metric_key} did not increase: {series[metric_key]}")


def assert_delta_zero(series: dict[str, dict[str, float]], metric_key: str) -> None:
    if abs(series[metric_key]["delta"]) > 1e-9:
        raise RuntimeError(f"{metric_key} changed unexpectedly: {series[metric_key]}")


def assert_after_at_least(series: dict[str, dict[str, float]], metric_key: str, minimum: float) -> None:
    if series[metric_key]["after"] < minimum:
        raise RuntimeError(f"{metric_key} stayed below {minimum}: {series[metric_key]}")


def write_report(report: dict[str, object], artifact_dir: Path | None) -> None:
    if artifact_dir is None:
        return
    artifact_dir.mkdir(parents=True, exist_ok=True)
    report_path = artifact_dir / "metrics-budget.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"metrics report: {report_path}")


def gateway_login_metric_specs() -> list[tuple[str, str, dict[str, str] | None]]:
    return [
        ("resume_routing_bind_total", "gateway_resume_routing_bind_total", None),
        ("resume_locator_bind_total", "gateway_resume_locator_bind_total", None),
    ]


def gateway_resume_metric_specs() -> list[tuple[str, str, dict[str, str] | None]]:
    return [
        ("sessions_active", "gateway_sessions_active", None),
        ("resume_routing_hit_total", "gateway_resume_routing_hit_total", None),
        ("resume_locator_lookup_hit_total", "gateway_resume_locator_lookup_hit_total", None),
        ("resume_locator_lookup_miss_total", "gateway_resume_locator_lookup_miss_total", None),
        ("resume_locator_selector_hit_total", "gateway_resume_locator_selector_hit_total", None),
        ("resume_locator_selector_fallback_total", "gateway_resume_locator_selector_fallback_total", None),
        ("world_policy_filtered_sticky_total", "gateway_world_policy_filtered_total", {"source": "sticky"}),
        ("world_policy_filtered_candidate_total", "gateway_world_policy_filtered_total", {"source": "candidate"}),
        ("world_policy_replacement_selected_total", "gateway_world_policy_replacement_selected_total", None),
    ]


def migration_server_metric_specs() -> list[tuple[str, str, dict[str, str] | None]]:
    return [
        ("chat_session_active", "chat_session_active", None),
        ("lease_resume_total", "chat_continuity_lease_resume_total", None),
        ("lease_resume_fail_total", "chat_continuity_lease_resume_fail_total", None),
        ("state_restore_total", "chat_continuity_state_restore_total", None),
        ("state_restore_fallback_total", "chat_continuity_state_restore_fallback_total", None),
        ("world_restore_total", "chat_continuity_world_restore_total", None),
        ("world_restore_fallback_total", "chat_continuity_world_restore_fallback_total", None),
        ("world_owner_restore_total", "chat_continuity_world_owner_restore_total", None),
        ("world_migration_restore_total", "chat_continuity_world_migration_restore_total", None),
        ("world_migration_restore_fallback_total", "chat_continuity_world_migration_restore_fallback_total", None),
    ]


def transfer_server_metric_specs() -> list[tuple[str, str, dict[str, str] | None]]:
    return [
        ("chat_session_active", "chat_session_active", None),
        ("lease_resume_total", "chat_continuity_lease_resume_total", None),
        ("lease_resume_fail_total", "chat_continuity_lease_resume_fail_total", None),
        ("state_restore_total", "chat_continuity_state_restore_total", None),
        ("state_restore_fallback_total", "chat_continuity_state_restore_fallback_total", None),
        ("world_restore_total", "chat_continuity_world_restore_total", None),
        ("world_restore_fallback_total", "chat_continuity_world_restore_fallback_total", None),
        ("world_owner_restore_total", "chat_continuity_world_owner_restore_total", None),
        ("world_owner_restore_fallback_total", "chat_continuity_world_owner_restore_fallback_total", None),
        ("world_migration_restore_total", "chat_continuity_world_migration_restore_total", None),
    ]


def wait_for_metrics_endpoint(port_forward: PortForward, timeout_seconds: float) -> str:
    deadline = time.time() + timeout_seconds
    last_error = "metrics port-forward not ready"
    while time.time() < deadline:
        try:
            port_forward.assert_running()
            wait_for_http_ok(port_forward.base_url, "/metrics", timeout_seconds=1.0)
            return port_forward.base_url
        except Exception as exc:
            last_error = str(exc)
            time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for metrics endpoint: {last_error}")


def run_default_migration_budget(
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
    room = f"kind_metrics_migration_room_{int(time.time())}"
    message = f"kind_metrics_migration_msg_{int(time.time() * 1000)}"
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
                "kind_metrics_migration",
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
            assert_delta_positive(resume_gateway_series, "world_policy_filtered_sticky_total")
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

            report = {
                "scenario": "default-migration-budget",
                "topology_config": str(DEFAULT_TOPOLOGY.relative_to(REPO_ROOT)),
                "login_gateway": "gateway-1",
                "resume_gateway": "gateway-2",
                "source_world_id": source_world_id,
                "target_world_id": target_world_id,
                "source_instance_id": source_instance_id,
                "target_instance_id": target_owner_instance_id,
                "logical_session_id": logical_session_id,
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
                "PASS kind-default-migration-budget: "
                f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} "
                f"migration_restore_delta={target_server_series['world_migration_restore_total']['delta']:.0f}"
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


def run_same_world_transfer_budget(
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
    room = f"kind_metrics_transfer_room_{int(time.time())}"
    message = f"kind_metrics_transfer_msg_{int(time.time() * 1000)}"
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
                "kind_metrics_transfer",
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
            assert_delta_positive(resume_gateway_series, "world_policy_filtered_sticky_total")
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

            report = {
                "scenario": "same-world-transfer-budget",
                "topology_config": str(SAME_WORLD_TOPOLOGY.relative_to(REPO_ROOT)),
                "login_gateway": "gateway-1",
                "resume_gateway": "gateway-2",
                "world_id": source_world_id,
                "source_instance_id": source_instance_id,
                "target_instance_id": replacement_owner_instance_id,
                "logical_session_id": logical_session_id,
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
                "PASS kind-same-world-transfer-budget: "
                f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} "
                f"owner_restore_delta={target_server_series['world_owner_restore_total']['delta']:.0f}"
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
    with tempfile.TemporaryDirectory(prefix=f"dynaxis-kind-metrics-{scenario_name}-") as temp_dir_raw:
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

                        if scenario_name == "default-migration-budget":
                            return run_default_migration_budget(
                                cluster_name,
                                namespace,
                                base_url,
                                login_host,
                                login_port,
                                resume_host,
                                resume_port,
                            )
                        if scenario_name == "same-world-transfer-budget":
                            return run_same_world_transfer_budget(
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
        description="Run live kind metrics-budget proofs for migration and transfer continuity flows."
    )
    parser.add_argument(
        "--scenario",
        choices=("matrix", "default-migration-budget", "same-world-transfer-budget"),
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
        if args.scenario in {"matrix", "default-migration-budget"}:
            reports.append(
                run_stage(
                    cluster_name=f"dynaxis-metrics-default-{int(time.time())}",
                    namespace="dynaxis-metrics-default",
                    topology_config=DEFAULT_TOPOLOGY,
                    scenario_name="default-migration-budget",
                    timeout_seconds=args.wait_timeout_seconds,
                )
            )
        if args.scenario in {"matrix", "same-world-transfer-budget"}:
            reports.append(
                run_stage(
                    cluster_name=f"dynaxis-metrics-sameworld-{int(time.time())}",
                    namespace="dynaxis-metrics-sameworld",
                    topology_config=SAME_WORLD_TOPOLOGY,
                    scenario_name="same-world-transfer-budget",
                    timeout_seconds=args.wait_timeout_seconds,
                )
            )
        report = {
            "proof": "worlds-kubernetes-kind-metrics-budget",
            "scenario": args.scenario,
            "scenarios": reports,
        }
        write_report(report, args.artifact_dir)
        print("PASS worlds-kubernetes-kind-metrics-budget")
        return 0
    except Exception as exc:
        print(f"FAIL worlds-kubernetes-kind-metrics-budget: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
