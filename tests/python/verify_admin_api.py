import json
import os
import time
import urllib.error
import urllib.request

from stack_topology import load_stack_topology
from stack_topology import first_server_for_other_world
from stack_topology import same_world_peer


BASE_URL = "http://127.0.0.1:39200"
STACK_TOPOLOGY = load_stack_topology()


def http_request(
    path: str, method: str = "GET", headers=None, data: bytes | None = None
):
    req = urllib.request.Request(
        f"{BASE_URL}{path}", method=method, headers=headers or {}, data=data
    )
    with urllib.request.urlopen(req, timeout=5) as response:
        return response.status, response.getheader("Content-Type", ""), response.read()


def wait_for_ready(path: str, timeout_sec: float = 30.0):
    deadline = time.time() + timeout_sec
    last_error = None

    while time.time() < deadline:
        try:
            status, _, body = http_request(path)
            if status == 200:
                return body
            last_error = f"unexpected status={status}"
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.5)

    raise RuntimeError(f"timeout waiting for {path}: {last_error}")


def load_json(path: str):
    status, content_type, body = http_request(path)
    if status != 200:
        raise RuntimeError(f"{path} expected 200, got {status}")
    if "application/json" not in content_type:
        raise RuntimeError(f"{path} expected json content-type, got {content_type}")
    return json.loads(body.decode("utf-8"))


def load_text(path: str):
    status, content_type, body = http_request(path)
    if status != 200:
        raise RuntimeError(f"{path} expected 200, got {status}")
    if "text/plain" not in content_type:
        raise RuntimeError(
            f"{path} expected text/plain content-type, got {content_type}"
        )
    return body.decode("utf-8", errors="replace")


def parse_prometheus_counter(metrics_text: str, metric_name: str) -> int:
    for line in metrics_text.splitlines():
        if not line or line.startswith("#"):
            continue
        if not line.startswith(metric_name):
            continue
        tail = line[len(metric_name) :]
        if tail and tail[0] not in (" ", "\t"):
            continue
        value = tail.strip().split(" ", 1)[0]
        if value:
            return int(float(value))
    raise RuntimeError(f"metric not found: {metric_name}")


def request_json(path: str, method: str = "GET"):
    try:
        status, content_type, body = http_request(path, method=method)
    except urllib.error.HTTPError as exc:
        status = exc.code
        content_type = exc.headers.get("Content-Type", "")
        body = exc.read()

    payload = None
    if body:
        payload = json.loads(body.decode("utf-8", errors="replace"))
    return status, content_type, payload


def request_json_body(
    path: str, method: str, body_obj, content_type: str = "application/json"
):
    payload_bytes = (
        body_obj
        if isinstance(body_obj, (bytes, bytearray))
        else json.dumps(body_obj).encode("utf-8")
    )
    payload_bytes = bytes(payload_bytes)
    headers = {
        "Content-Type": content_type,
        "Content-Length": str(len(payload_bytes)),
    }
    try:
        status, content_type_response, body = http_request(
            path, method=method, headers=headers, data=payload_bytes
        )
    except urllib.error.HTTPError as exc:
        status = exc.code
        content_type_response = exc.headers.get("Content-Type", "")
        body = exc.read()

    payload = None
    if body:
        payload = json.loads(body.decode("utf-8", errors="replace"))
    return status, content_type_response, payload


def wait_for_instances(timeout_sec: float = 30.0):
    deadline = time.time() + timeout_sec
    last_error = None

    while time.time() < deadline:
        try:
            payload = load_json("/api/v1/instances?limit=1")
            items = payload.get("data", {}).get("items", [])
            if items:
                return payload
            last_error = "instances list is empty"
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.5)

    raise RuntimeError(f"timeout waiting for instances: {last_error}")


def wait_for_deployment(command_id: str, timeout_sec: float = 30.0):
    deadline = time.time() + timeout_sec
    last_status = ""
    while time.time() < deadline:
        payload = load_json("/api/v1/ext/deployments?limit=100")
        for item in payload.get("data", {}).get("items", []):
            if not isinstance(item, dict):
                continue
            if item.get("command_id") != command_id:
                continue
            status = item.get("status", "")
            last_status = status
            if status in ("completed", "failed", "cancelled"):
                return item
        time.sleep(0.5)
    raise RuntimeError(
        f"timeout waiting deployment command_id={command_id}, last_status={last_status}"
    )


def main() -> int:
    try:
        wait_for_ready("/healthz")
        wait_for_ready("/readyz")

        status, content_type, body = http_request("/admin")
        if status != 200:
            raise RuntimeError(f"/admin expected 200, got {status}")
        if "text/html" not in content_type:
            raise RuntimeError(f"/admin expected html content-type, got {content_type}")
        html = body.decode("utf-8", errors="replace")
        if "Dynaxis Admin Console" not in html:
            raise RuntimeError("/admin missing expected UI title")
        if 'id="audit-trend"' not in html:
            raise RuntimeError("/admin missing audit trend container")
        if "HTTP 5xx" not in html:
            raise RuntimeError("/admin missing HTTP 5xx overview card label")
        if "World Lifecycle" not in html:
            raise RuntimeError("/admin missing world lifecycle section")

        overview = load_json("/api/v1/overview")
        data = overview.get("data", {})
        if data.get("service") != "admin_app":
            raise RuntimeError("overview.service mismatch")
        if data.get("mode") != "control_plane":
            raise RuntimeError("overview.mode mismatch")
        services = data.get("services", {})
        for key in ("gateway", "server", "wb_worker", "haproxy"):
            if key not in services:
                raise RuntimeError(f"overview.services missing '{key}'")

        counts = data.get("counts", {})
        for key in (
            "instances_total",
            "instances_ready",
            "instances_not_ready",
            "http_errors_total",
            "http_server_errors_total",
            "http_unauthorized_total",
            "http_forbidden_total",
        ):
            if key not in counts:
                raise RuntimeError(f"overview.counts missing '{key}'")
            if not isinstance(counts[key], int):
                raise RuntimeError(f"overview.counts.{key} expected int")

        audit_trend = data.get("audit_trend", {})
        if "step_ms" not in audit_trend or "max_points" not in audit_trend:
            raise RuntimeError("overview.audit_trend missing step_ms/max_points")
        if not isinstance(audit_trend["step_ms"], int) or audit_trend["step_ms"] <= 0:
            raise RuntimeError("overview.audit_trend.step_ms expected positive int")
        if (
            not isinstance(audit_trend["max_points"], int)
            or audit_trend["max_points"] <= 0
        ):
            raise RuntimeError("overview.audit_trend.max_points expected positive int")
        points = audit_trend.get("points")
        if not isinstance(points, list):
            raise RuntimeError("overview.audit_trend.points expected list")
        if points:
            sample = points[-1]
            for key in (
                "timestamp_ms",
                "http_errors_total",
                "http_server_errors_total",
                "http_unauthorized_total",
                "http_forbidden_total",
            ):
                if key not in sample:
                    raise RuntimeError(
                        f"overview.audit_trend.points sample missing '{key}'"
                    )

        if "meta" not in overview or "request_id" not in overview["meta"]:
            raise RuntimeError("overview meta.request_id missing")

        force_fail_wave_index = int(
            os.environ.get("ADMIN_EXT_FORCE_FAIL_WAVE_INDEX", "0") or "0"
        )

        auth_context = load_json("/api/v1/auth/context")
        auth_data = auth_context.get("data", {})
        if auth_data.get("mode") != "off":
            raise RuntimeError("auth context mode mismatch")
        if auth_data.get("actor") != "anonymous":
            raise RuntimeError("auth context actor mismatch")
        role = auth_data.get("role")
        if role not in ("viewer", "operator", "admin"):
            raise RuntimeError("auth context role mismatch")
        capabilities = auth_data.get("capabilities", {})
        for key in ("disconnect", "announce", "settings", "moderation", "world_policy", "world_drain", "world_transfer", "world_migration", "ext_deploy"):
            if key not in capabilities:
                raise RuntimeError(f"auth context capabilities missing '{key}'")
            if not isinstance(capabilities[key], bool):
                raise RuntimeError(f"auth context capabilities.{key} expected bool")

        ext_inventory = load_json("/api/v1/ext/inventory")
        ext_data = ext_inventory.get("data", {})
        ext_items = ext_data.get("items")
        if not isinstance(ext_items, list):
            raise RuntimeError("ext inventory payload missing items[]")

        status, _, payload = request_json_body(
            "/api/v1/ext/precheck",
            method="POST",
            body_obj={
                "artifact_id": "plugin:missing-command-id",
            },
        )
        if status != 400:
            raise RuntimeError(
                f"POST /api/v1/ext/precheck missing command_id expected 400, got {status}"
            )
        if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
            raise RuntimeError("precheck bad request expected BAD_REQUEST")

        candidate = None
        for item in ext_items:
            if not isinstance(item, dict):
                continue
            if not item.get("artifact_id"):
                continue
            issues = item.get("issues")
            if isinstance(issues, list) and len(issues) == 0:
                candidate = item
                break

        ext_cmd = f"admin-ext-{int(time.time())}"
        if candidate is None:
            status, _, payload = request_json_body(
                "/api/v1/ext/precheck",
                method="POST",
                body_obj={
                    "command_id": ext_cmd,
                    "artifact_id": "plugin:not-found",
                    "selector": {"all": True},
                    "rollout_strategy": {"type": "all_at_once"},
                },
            )
            if status != 409:
                raise RuntimeError(
                    f"ext precheck unknown artifact expected 409, got {status}"
                )
            if (payload or {}).get("error", {}).get("code") != "PRECHECK_FAILED":
                raise RuntimeError(
                    "ext precheck unknown artifact expected PRECHECK_FAILED"
                )
        else:
            artifact_id = candidate["artifact_id"]

            instances_payload = load_json("/api/v1/instances?limit=100")
            instance_items = instances_payload.get("data", {}).get("items", [])
            if not isinstance(instance_items, list) or not instance_items:
                raise RuntimeError(
                    "instances payload missing items for selector deployment checks"
                )
            single_instance_id = instance_items[0].get("instance_id")
            single_role = instance_items[0].get("role")
            single_shard = instance_items[0].get("shard")
            if not single_instance_id:
                raise RuntimeError(
                    "instances payload missing instance_id for selector checks"
                )
            if not single_role:
                raise RuntimeError("instances payload missing role for selector checks")
            if not single_shard:
                raise RuntimeError(
                    "instances payload missing shard for selector checks"
                )

            status, _, payload = request_json_body(
                "/api/v1/ext/precheck",
                method="POST",
                body_obj={
                    "command_id": ext_cmd,
                    "artifact_id": artifact_id,
                    "selector": {"all": True},
                    "run_at_utc": None,
                    "rollout_strategy": {"type": "all_at_once"},
                },
            )
            if status != 200:
                raise RuntimeError(f"ext precheck expected 200, got {status}")
            if (payload or {}).get("data", {}).get("status") != "precheck_passed":
                raise RuntimeError("ext precheck expected precheck_passed")

            status, _, payload = request_json_body(
                "/api/v1/ext/deployments",
                method="POST",
                body_obj={
                    "command_id": f"{ext_cmd}-single",
                    "artifact_id": artifact_id,
                    "selector": {"server_ids": [single_instance_id]},
                    "rollout_strategy": {"type": "all_at_once"},
                },
            )
            if status != 202:
                raise RuntimeError(
                    f"ext single-target deployment expected 202, got {status}"
                )
            single_item = wait_for_deployment(f"{ext_cmd}-single", timeout_sec=30.0)
            if single_item.get("status") != "completed":
                raise RuntimeError("ext single-target deployment expected completed")

            status, _, payload = request_json_body(
                "/api/v1/ext/deployments",
                method="POST",
                body_obj={
                    "command_id": f"{ext_cmd}-role",
                    "artifact_id": artifact_id,
                    "selector": {"roles": [single_role]},
                    "rollout_strategy": {"type": "all_at_once"},
                },
            )
            if status != 202:
                raise RuntimeError(
                    f"ext role-target deployment expected 202, got {status}"
                )
            role_item = wait_for_deployment(f"{ext_cmd}-role", timeout_sec=30.0)
            if role_item.get("status") != "completed":
                raise RuntimeError("ext role-target deployment expected completed")

            status, _, payload = request_json_body(
                "/api/v1/ext/deployments",
                method="POST",
                body_obj={
                    "command_id": f"{ext_cmd}-shard",
                    "artifact_id": artifact_id,
                    "selector": {"shards": [single_shard]},
                    "rollout_strategy": {"type": "all_at_once"},
                },
            )
            if status != 202:
                raise RuntimeError(
                    f"ext shard-target deployment expected 202, got {status}"
                )
            shard_item = wait_for_deployment(f"{ext_cmd}-shard", timeout_sec=30.0)
            if shard_item.get("status") != "completed":
                raise RuntimeError("ext shard-target deployment expected completed")

            status, _, payload = request_json_body(
                "/api/v1/ext/deployments",
                method="POST",
                body_obj={
                    "command_id": ext_cmd,
                    "artifact_id": artifact_id,
                    "selector": {"all": True},
                    "rollout_strategy": {"type": "all_at_once"},
                },
            )
            if status != 202:
                raise RuntimeError(f"ext deployment expected 202, got {status}")
            if (payload or {}).get("data", {}).get("status") != "pending":
                raise RuntimeError("ext deployment expected pending status")
            all_item = wait_for_deployment(ext_cmd, timeout_sec=30.0)
            if all_item.get("status") != "completed":
                raise RuntimeError("ext all-target deployment expected completed")

            status, _, payload = request_json_body(
                "/api/v1/ext/deployments",
                method="POST",
                body_obj={
                    "command_id": ext_cmd,
                    "artifact_id": artifact_id,
                    "selector": {"all": True},
                    "rollout_strategy": {"type": "all_at_once"},
                },
            )
            if status != 409:
                raise RuntimeError(
                    f"ext deployment duplicate command_id expected 409, got {status}"
                )
            if (payload or {}).get("error", {}).get("code") != "IDEMPOTENT_REJECTED":
                raise RuntimeError(
                    "ext deployment duplicate expected IDEMPOTENT_REJECTED"
                )

            status, _, payload = request_json_body(
                "/api/v1/ext/schedules",
                method="POST",
                body_obj={
                    "command_id": f"{ext_cmd}-missing-run-at",
                    "artifact_id": artifact_id,
                    "selector": {"all": True},
                    "rollout_strategy": {"type": "canary_wave"},
                },
            )
            if status != 400:
                raise RuntimeError(
                    f"ext schedule without run_at_utc expected 400, got {status}"
                )

            status, _, payload = request_json_body(
                "/api/v1/ext/schedules",
                method="POST",
                body_obj={
                    "command_id": f"{ext_cmd}-schedule",
                    "artifact_id": artifact_id,
                    "selector": {"all": True},
                    "run_at_utc": int(time.time() * 1000) + 15000,
                    "rollout_strategy": {
                        "type": "canary_wave",
                        "waves": [5, 25, 100],
                        "rollback_on_failure": True,
                    },
                },
            )
            if status != 202:
                raise RuntimeError(f"ext schedule expected 202, got {status}")

            deployments_payload = load_json("/api/v1/ext/deployments?limit=10")
            dep_items = deployments_payload.get("data", {}).get("items")
            if not isinstance(dep_items, list):
                raise RuntimeError("ext deployments payload missing items[]")
            if not any(
                it.get("command_id") == ext_cmd
                for it in dep_items
                if isinstance(it, dict)
            ):
                raise RuntimeError("ext deployments list missing submitted command_id")

            if force_fail_wave_index > 0:
                instance_probe = load_json("/api/v1/instances?limit=100")
                instance_count = len(instance_probe.get("data", {}).get("items", []))
                if instance_count < 2:
                    raise RuntimeError(
                        f"ext canary forced-failure requires >=2 instances, got {instance_count}"
                    )

                metrics_before = load_text("/metrics")
                rollbacks_before = parse_prometheus_counter(
                    metrics_before, "admin_ext_rollbacks_total"
                )

                failed_cmd = f"{ext_cmd}-canary-fail"
                status, _, payload = request_json_body(
                    "/api/v1/ext/deployments",
                    method="POST",
                    body_obj={
                        "command_id": failed_cmd,
                        "artifact_id": artifact_id,
                        "selector": {"all": True},
                        "rollout_strategy": {
                            "type": "canary_wave",
                            "waves": [50, 100],
                            "rollback_on_failure": True,
                        },
                    },
                )
                if status != 202:
                    raise RuntimeError(
                        f"ext canary deployment expected 202, got {status}"
                    )

                failed_item = wait_for_deployment(failed_cmd, timeout_sec=30.0)
                if failed_item.get("status") != "failed":
                    raise RuntimeError(
                        "ext canary forced failure expected failed status"
                    )
                if failed_item.get("status_reason") != "wave_forced_failure":
                    raise RuntimeError(
                        "ext canary forced failure expected wave_forced_failure"
                    )
                issues = failed_item.get("issues", [])
                if not any(
                    isinstance(issue, dict)
                    and issue.get("code") == "wave_forced_failure"
                    for issue in issues
                ):
                    raise RuntimeError("ext canary forced failure issue missing")

                metrics_after = load_text("/metrics")
                rollbacks_after = parse_prometheus_counter(
                    metrics_after, "admin_ext_rollbacks_total"
                )
                if rollbacks_after < rollbacks_before + 1:
                    raise RuntimeError(
                        "ext canary forced failure expected admin_ext_rollbacks_total increment"
                    )

        moderation_paths = [
            "/api/v1/users/mute?client_id=admin_api_probe&duration_sec=30&reason=api-check",
            "/api/v1/users/unmute?client_id=admin_api_probe&reason=api-check",
            "/api/v1/users/ban?client_id=admin_api_probe&duration_sec=60&reason=api-check",
            "/api/v1/users/unban?client_id=admin_api_probe&reason=api-check",
            "/api/v1/users/kick?client_id=admin_api_probe&reason=api-check",
        ]
        for path in moderation_paths:
            status, _, payload = request_json(path, method="POST")
            if status != 200:
                raise RuntimeError(f"{path} expected 200, got {status}")
            if (payload or {}).get("data", {}).get("accepted") is not True:
                raise RuntimeError(f"{path} missing accepted=true")

        status, _, payload = request_json_body(
            "/api/v1/users/disconnect",
            method="POST",
            body_obj={
                "client_ids": ["admin_api_probe", "admin_api_probe_2"],
                "reason": "api-json-body-check",
            },
        )
        if status != 200:
            raise RuntimeError(
                f"POST /api/v1/users/disconnect(json) expected 200, got {status}"
            )
        if (payload or {}).get("data", {}).get("submitted_count") != 2:
            raise RuntimeError("disconnect json body submitted_count mismatch")

        status, _, payload = request_json_body(
            "/api/v1/settings",
            method="PATCH",
            body_obj={
                "key": "chat_spam_threshold",
                "value": 7,
            },
        )
        if status != 200:
            raise RuntimeError(
                f"PATCH /api/v1/settings(json) expected 200, got {status}"
            )
        if (payload or {}).get("data", {}).get("key") != "chat_spam_threshold":
            raise RuntimeError("settings json body key mismatch")

        status, _, payload = request_json_body(
            "/api/v1/settings",
            method="PATCH",
            body_obj=b"{",
            content_type="application/json",
        )
        if status != 400:
            raise RuntimeError(
                f"PATCH /api/v1/settings malformed json expected 400, got {status}"
            )
        if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
            raise RuntimeError("malformed json expected BAD_REQUEST")

        status, _, payload = request_json_body(
            "/api/v1/settings",
            method="PATCH",
            body_obj="key=chat_spam_threshold&value=7",
            content_type="text/plain",
        )
        if status != 415:
            raise RuntimeError(
                f"PATCH /api/v1/settings unsupported content type expected 415, got {status}"
            )
        if (payload or {}).get("error", {}).get("code") != "UNSUPPORTED_CONTENT_TYPE":
            raise RuntimeError(
                "unsupported content type expected UNSUPPORTED_CONTENT_TYPE"
            )

        users_payload = load_json("/api/v1/users?limit=10")
        users_data = users_payload.get("data", {})
        if "items" not in users_data or "paging" not in users_data:
            raise RuntimeError("users payload missing items/paging")

        instances = wait_for_instances()
        instances_data = instances.get("data", {})
        items = instances_data.get("items", [])
        paging = instances_data.get("paging", {})
        if paging.get("limit") != 1:
            raise RuntimeError("instances paging.limit mismatch")
        if not items:
            raise RuntimeError("instances endpoint returned empty items")

        instance_id = items[0].get("instance_id", "")
        if not instance_id:
            raise RuntimeError("instances item missing instance_id")

        full_instances = load_json("/api/v1/instances?limit=100")
        full_items = full_instances.get("data", {}).get("items", [])
        if not isinstance(full_items, list) or not full_items:
            raise RuntimeError("full instances payload missing items")
        world_server = None
        for item in full_items:
            if not isinstance(item, dict):
                continue
            world_scope = item.get("world_scope")
            if item.get("role") == "server" and isinstance(world_scope, dict):
                if world_scope.get("world_id"):
                    world_server = item
                    break
        if world_server is None:
            raise RuntimeError("instances payload missing server world_scope data")
        world_scope = world_server.get("world_scope", {})
        if "owner_instance_id" not in world_scope:
            raise RuntimeError("instances world_scope missing owner_instance_id")
        if not isinstance(world_scope.get("owner_match"), bool):
            raise RuntimeError("instances world_scope.owner_match expected bool")
        policy = world_scope.get("policy", {})
        if not isinstance(policy, dict):
            raise RuntimeError("instances world_scope.policy missing")
        if not isinstance(policy.get("draining"), bool):
            raise RuntimeError("instances world_scope.policy.draining expected bool")
        if not isinstance(policy.get("reassignment_declared"), bool):
            raise RuntimeError(
                "instances world_scope.policy.reassignment_declared expected bool"
            )
        source = world_scope.get("source", {})
        if not isinstance(source, dict) or not source.get("owner_key"):
            raise RuntimeError("instances world_scope.source.owner_key missing")
        if not source.get("policy_key"):
            raise RuntimeError("instances world_scope.source.policy_key missing")

        detail = load_json(f"/api/v1/instances/{instance_id}")
        detail_data = detail.get("data", {})
        if not detail_data.get("metrics_url"):
            raise RuntimeError("instance detail missing metrics_url")
        if not detail_data.get("ready_reason"):
            raise RuntimeError("instance detail missing ready_reason")

        world_detail = load_json(f"/api/v1/instances/{world_server['instance_id']}")
        world_detail_data = world_detail.get("data", {})
        detail_scope = world_detail_data.get("world_scope")
        if not isinstance(detail_scope, dict):
            raise RuntimeError("instance detail missing world_scope object")
        if detail_scope.get("world_id") != world_scope.get("world_id"):
            raise RuntimeError("instance detail world_scope.world_id mismatch")
        if "owner_instance_id" not in detail_scope:
            raise RuntimeError("instance detail world_scope missing owner_instance_id")
        if not isinstance(detail_scope.get("owner_match"), bool):
            raise RuntimeError("instance detail world_scope.owner_match expected bool")
        detail_policy = detail_scope.get("policy", {})
        if not isinstance(detail_policy, dict):
            raise RuntimeError("instance detail world_scope.policy missing")
        if not isinstance(detail_policy.get("draining"), bool):
            raise RuntimeError("instance detail world_scope.policy.draining expected bool")
        if not isinstance(detail_policy.get("reassignment_declared"), bool):
            raise RuntimeError(
                "instance detail world_scope.policy.reassignment_declared expected bool"
            )
        detail_source = detail_scope.get("source", {})
        if not isinstance(detail_source, dict) or not detail_source.get("owner_key"):
            raise RuntimeError("instance detail world_scope.source.owner_key missing")
        if not detail_source.get("policy_key"):
            raise RuntimeError("instance detail world_scope.source.policy_key missing")

        replacement_peer = same_world_peer(
            STACK_TOPOLOGY,
            str(world_server["instance_id"]),
            str(world_scope.get("world_id")),
        )
        replacement_owner = (
            str(replacement_peer["instance_id"])
            if replacement_peer is not None
            else str(world_server["instance_id"])
        )
        world_policy_path = f"/api/v1/worlds/{world_scope.get('world_id')}/policy"
        metrics_before = load_text("/metrics")
        world_policy_write_before = parse_prometheus_counter(
            metrics_before, "admin_world_policy_write_total"
        )
        world_policy_write_fail_before = parse_prometheus_counter(
            metrics_before, "admin_world_policy_write_fail_total"
        )
        world_policy_clear_before = parse_prometheus_counter(
            metrics_before, "admin_world_policy_clear_total"
        )
        world_policy_clear_fail_before = parse_prometheus_counter(
            metrics_before, "admin_world_policy_clear_fail_total"
        )
        world_policy_write_after_put = world_policy_write_before
        try:
            status, _, payload = request_json_body(
                world_policy_path,
                method="PUT",
                body_obj={
                    "draining": True,
                    "replacement_owner_instance_id": replacement_owner,
                },
            )
            if status != 200:
                raise RuntimeError(
                    f"PUT {world_policy_path} expected 200, got {status}"
                )
            policy_response = (payload or {}).get("data", {})
            if policy_response.get("world_id") != world_scope.get("world_id"):
                raise RuntimeError("world policy PUT response world_id mismatch")
            if policy_response.get("owner_instance_id") != world_scope.get("owner_instance_id"):
                raise RuntimeError("world policy PUT response owner_instance_id mismatch")
            response_policy = policy_response.get("policy", {})
            if response_policy.get("draining") is not True:
                raise RuntimeError("world policy PUT response draining mismatch")
            if response_policy.get("replacement_owner_instance_id") != replacement_owner:
                raise RuntimeError(
                    "world policy PUT response replacement_owner_instance_id mismatch"
                )
            response_source = policy_response.get("source", {})
            if response_source.get("policy_key") != detail_source["policy_key"]:
                raise RuntimeError("world policy PUT response policy_key mismatch")

            metrics_after_put = load_text("/metrics")
            world_policy_write_after_put = parse_prometheus_counter(
                metrics_after_put, "admin_world_policy_write_total"
            )
            if world_policy_write_after_put <= world_policy_write_before:
                raise RuntimeError(
                    "admin world policy write metric did not increase after PUT"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_put, "admin_world_policy_write_fail_total"
                )
                != world_policy_write_fail_before
            ):
                raise RuntimeError(
                    "admin world policy write_fail metric changed after successful PUT"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_put, "admin_world_policy_clear_total"
                )
                != world_policy_clear_before
            ):
                raise RuntimeError(
                    "admin world policy clear metric changed before DELETE"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_put, "admin_world_policy_clear_fail_total"
                )
                != world_policy_clear_fail_before
            ):
                raise RuntimeError(
                    "admin world policy clear_fail metric changed after successful PUT"
                )

            updated_instances = load_json("/api/v1/instances?limit=100")
            updated_items = updated_instances.get("data", {}).get("items", [])
            updated_world_server = next(
                (
                    item
                    for item in updated_items
                    if item.get("instance_id") == world_server["instance_id"]
                ),
                None,
            )
            if not isinstance(updated_world_server, dict):
                raise RuntimeError("updated instances payload missing world server item")
            updated_policy = updated_world_server.get("world_scope", {}).get("policy", {})
            if updated_policy.get("draining") is not True:
                raise RuntimeError("instances world_scope policy.draining was not refreshed")
            if updated_policy.get("replacement_owner_instance_id") != replacement_owner:
                raise RuntimeError(
                    "instances world_scope policy.replacement_owner_instance_id mismatch"
                )

            worlds = load_json("/api/v1/worlds?limit=100")
            world_items = worlds.get("data", {}).get("items", [])
            world_item = next(
                (
                    item
                    for item in world_items
                    if item.get("world_id") == world_scope.get("world_id")
                ),
                None,
            )
            if not isinstance(world_item, dict):
                raise RuntimeError("worlds payload missing world inventory item")
            if world_item.get("owner_instance_id") != world_scope.get("owner_instance_id"):
                raise RuntimeError("world inventory owner_instance_id mismatch")
            world_policy = world_item.get("policy", {})
            if world_policy.get("draining") is not True:
                raise RuntimeError("world inventory policy.draining mismatch")
            if world_policy.get("replacement_owner_instance_id") != replacement_owner:
                raise RuntimeError(
                    "world inventory replacement_owner_instance_id mismatch"
                )
            world_instances = world_item.get("instances", [])
            if not isinstance(world_instances, list) or not any(
                isinstance(entry, dict)
                and entry.get("instance_id") == world_server["instance_id"]
                and isinstance(entry.get("owner_match"), bool)
                for entry in world_instances
            ):
                raise RuntimeError("world inventory instances missing owner_match state")
            world_source = world_item.get("source", {})
            if (
                not isinstance(world_source, dict)
                or world_source.get("policy_key") != detail_source["policy_key"]
            ):
                raise RuntimeError("world inventory source.policy_key mismatch")
            world_transfer = world_item.get("transfer", {})
            if not isinstance(world_transfer, dict):
                raise RuntimeError("world inventory transfer payload missing")
            expected_transfer_phase = (
                "owner_missing"
                if world_scope.get("owner_instance_id") in (None, "")
                else (
                    "owner_handoff_committed"
                    if replacement_owner == world_scope.get("owner_instance_id")
                    else "awaiting_owner_handoff"
                )
            )
            if world_transfer.get("phase") != expected_transfer_phase:
                raise RuntimeError("world inventory transfer phase mismatch after policy PUT")
            transfer_summary = world_transfer.get("summary", {})
            if transfer_summary.get("transfer_declared") is not True:
                raise RuntimeError("world inventory transfer summary.transfer_declared mismatch")

            status, _, payload = request_json_body(
                "/api/v1/worlds/not-a-real-world/policy",
                method="PUT",
                body_obj={
                    "draining": True,
                    "replacement_owner_instance_id": replacement_owner,
                },
            )
            if status != 400:
                raise RuntimeError(
                    f"PUT invalid world_id expected 400, got {status}"
                )
            if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
                raise RuntimeError("invalid world_id expected BAD_REQUEST")

            status, _, payload = request_json_body(
                world_policy_path,
                method="PUT",
                body_obj={
                    "draining": True,
                    "replacement_owner_instance_id": "not-a-real-instance",
                },
            )
            if status != 400:
                raise RuntimeError(
                    f"PUT invalid replacement owner expected 400, got {status}"
                )
            if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
                raise RuntimeError("invalid replacement owner expected BAD_REQUEST")

            metrics_after_invalid = load_text("/metrics")
            if (
                parse_prometheus_counter(
                    metrics_after_invalid, "admin_world_policy_write_total"
                )
                != world_policy_write_after_put
            ):
                raise RuntimeError(
                    "admin world policy write metric changed after validation failures"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_invalid, "admin_world_policy_write_fail_total"
                )
                != world_policy_write_fail_before
            ):
                raise RuntimeError(
                    "admin world policy write_fail metric changed after validation failures"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_invalid, "admin_world_policy_clear_total"
                )
                != world_policy_clear_before
            ):
                raise RuntimeError(
                    "admin world policy clear metric changed after validation failures"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_invalid, "admin_world_policy_clear_fail_total"
                )
                != world_policy_clear_fail_before
            ):
                raise RuntimeError(
                    "admin world policy clear_fail metric changed after validation failures"
                )
        finally:
            status, _, payload = request_json(world_policy_path, method="DELETE")
            if status != 200:
                raise RuntimeError(
                    f"DELETE {world_policy_path} expected 200, got {status}"
                )
            cleared = (payload or {}).get("data", {})
            cleared_policy = cleared.get("policy", {})
            if cleared_policy.get("draining") is not False:
                raise RuntimeError("world policy DELETE response draining mismatch")
            if cleared_policy.get("replacement_owner_instance_id") is not None:
                raise RuntimeError(
                    "world policy DELETE response replacement_owner_instance_id mismatch"
                )
            metrics_after_delete = load_text("/metrics")
            world_policy_clear_after = parse_prometheus_counter(
                metrics_after_delete, "admin_world_policy_clear_total"
            )
            if world_policy_clear_after <= world_policy_clear_before:
                raise RuntimeError(
                    "admin world policy clear metric did not increase after DELETE"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_delete, "admin_world_policy_write_total"
                )
                != world_policy_write_after_put
            ):
                raise RuntimeError(
                    "admin world policy write metric changed unexpectedly after DELETE"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_delete, "admin_world_policy_write_fail_total"
                )
                != world_policy_write_fail_before
            ):
                raise RuntimeError(
                    "admin world policy write_fail metric changed after DELETE"
                )
            if (
                parse_prometheus_counter(
                    metrics_after_delete, "admin_world_policy_clear_fail_total"
                )
                != world_policy_clear_fail_before
            ):
                raise RuntimeError(
                    "admin world policy clear_fail metric changed after DELETE"
                )

        cleared_instances = load_json("/api/v1/instances?limit=100")
        cleared_items = cleared_instances.get("data", {}).get("items", [])
        cleared_world_server = next(
            (
                item
                for item in cleared_items
                if item.get("instance_id") == world_server["instance_id"]
            ),
            None,
        )
        if not isinstance(cleared_world_server, dict):
            raise RuntimeError("cleared instances payload missing world server item")
        cleared_policy = cleared_world_server.get("world_scope", {}).get("policy", {})
        if cleared_policy.get("draining") is not False:
            raise RuntimeError("instances world_scope policy.draining did not clear")
        if cleared_policy.get("replacement_owner_instance_id") is not None:
            raise RuntimeError(
                "instances world_scope replacement_owner_instance_id did not clear"
            )
        cleared_worlds = load_json("/api/v1/worlds?limit=100")
        cleared_world_items = cleared_worlds.get("data", {}).get("items", [])
        cleared_world_item = next(
            (
                item
                for item in cleared_world_items
                if item.get("world_id") == world_scope.get("world_id")
            ),
            None,
        )
        if not isinstance(cleared_world_item, dict):
            raise RuntimeError("world inventory item missing after policy clear")
        cleared_world_policy = cleared_world_item.get("policy", {})
        if cleared_world_policy.get("draining") is not False:
            raise RuntimeError("world inventory policy.draining did not clear")
        if cleared_world_policy.get("replacement_owner_instance_id") is not None:
            raise RuntimeError(
                "world inventory replacement_owner_instance_id did not clear"
            )
        cleared_world_transfer = cleared_world_item.get("transfer", {})
        if not isinstance(cleared_world_transfer, dict):
            raise RuntimeError("world inventory transfer missing after policy clear")
        if cleared_world_transfer.get("phase") != "idle":
            raise RuntimeError("world inventory transfer phase did not reset after policy clear")

        world_drain_path = f"/api/v1/worlds/{world_scope.get('world_id')}/drain"
        metrics_before_world_drain = load_text("/metrics")
        world_drain_requests_before = parse_prometheus_counter(
            metrics_before_world_drain, "admin_world_drain_requests_total"
        )
        world_drain_write_before = parse_prometheus_counter(
            metrics_before_world_drain, "admin_world_drain_write_total"
        )
        world_drain_write_fail_before = parse_prometheus_counter(
            metrics_before_world_drain, "admin_world_drain_write_fail_total"
        )
        world_drain_clear_before = parse_prometheus_counter(
            metrics_before_world_drain, "admin_world_drain_clear_total"
        )
        world_drain_clear_fail_before = parse_prometheus_counter(
            metrics_before_world_drain, "admin_world_drain_clear_fail_total"
        )

        drain_initial = load_json(world_drain_path)
        drain_initial_data = drain_initial.get("data", {})
        drain_initial_state = drain_initial_data.get("drain", {})
        if drain_initial_state.get("phase") != "idle":
            raise RuntimeError("world drain GET should report idle before PUT")

        status, _, payload = request_json_body(
            world_drain_path,
            method="PUT",
            body_obj={
                "replacement_owner_instance_id": None,
            },
        )
        if status != 200:
            raise RuntimeError(f"world drain PUT expected 200, got {status}")
        drain_response = (payload or {}).get("data", {})
        drain_state = drain_response.get("drain", {})
        if drain_state.get("phase") != "drained":
            raise RuntimeError("world drain PUT phase mismatch")
        if drain_state.get("summary", {}).get("drain_declared") is not True:
            raise RuntimeError("world drain PUT summary.drain_declared mismatch")
        if drain_state.get("summary", {}).get("active_sessions_total") != 0:
            raise RuntimeError("world drain PUT active_sessions_total mismatch")
        orchestration_state = drain_state.get("orchestration", {})
        if orchestration_state.get("phase") != "ready_to_clear":
            raise RuntimeError("world drain PUT orchestration phase mismatch")
        if orchestration_state.get("next_action") != "clear_policy":
            raise RuntimeError("world drain PUT orchestration next_action mismatch")
        if orchestration_state.get("summary", {}).get("clear_allowed") is not True:
            raise RuntimeError("world drain PUT orchestration clear_allowed mismatch")

        metrics_after_world_drain_put = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_put, "admin_world_drain_requests_total"
            )
            <= world_drain_requests_before
        ):
            raise RuntimeError("admin world drain requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_put, "admin_world_drain_write_total"
            )
            <= world_drain_write_before
        ):
            raise RuntimeError("admin world drain write metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_put, "admin_world_drain_write_fail_total"
            )
            != world_drain_write_fail_before
        ):
            raise RuntimeError("admin world drain write_fail metric changed after successful PUT")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_put, "admin_world_drain_clear_total"
            )
            != world_drain_clear_before
        ):
            raise RuntimeError("admin world drain clear metric changed before DELETE")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_put, "admin_world_drain_clear_fail_total"
            )
            != world_drain_clear_fail_before
        ):
            raise RuntimeError("admin world drain clear_fail metric changed before DELETE")

        drained_worlds = load_json("/api/v1/worlds?limit=100")
        drained_world_item = next(
            (
                item
                for item in drained_worlds.get("data", {}).get("items", [])
                if item.get("world_id") == world_scope.get("world_id")
            ),
            None,
        )
        if not isinstance(drained_world_item, dict):
            raise RuntimeError("world inventory item missing after world drain PUT")
        if drained_world_item.get("drain", {}).get("phase") != "drained":
            raise RuntimeError("world inventory drain phase mismatch after PUT")
        if drained_world_item.get("drain", {}).get("orchestration", {}).get("phase") != "ready_to_clear":
            raise RuntimeError("world inventory drain orchestration phase mismatch after PUT")

        status, _, payload = request_json_body(
            world_drain_path,
            method="PUT",
            body_obj={
                "replacement_owner_instance_id": "not-a-real-instance",
            },
        )
        if status != 400:
            raise RuntimeError(f"world drain invalid target expected 400, got {status}")
        if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
            raise RuntimeError("world drain invalid target expected BAD_REQUEST")

        metrics_after_world_drain_invalid = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_invalid, "admin_world_drain_write_total"
            )
            != parse_prometheus_counter(
                metrics_after_world_drain_put, "admin_world_drain_write_total"
            )
        ):
            raise RuntimeError("admin world drain write metric changed after invalid PUTs")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_invalid, "admin_world_drain_write_fail_total"
            )
            < world_drain_write_fail_before + 1
        ):
            raise RuntimeError("admin world drain write_fail metric did not increase after invalid PUTs")

        status, _, payload = request_json(world_drain_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"world drain DELETE expected 200, got {status}")
        drain_cleared = (payload or {}).get("data", {})
        if drain_cleared.get("drain", {}).get("phase") != "idle":
            raise RuntimeError("world drain DELETE should reset phase to idle")

        metrics_after_world_drain_delete = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_delete, "admin_world_drain_clear_total"
            )
            <= world_drain_clear_before
        ):
            raise RuntimeError("admin world drain clear metric did not increase after DELETE")
        if (
            parse_prometheus_counter(
                metrics_after_world_drain_delete, "admin_world_drain_clear_fail_total"
            )
            != world_drain_clear_fail_before
        ):
            raise RuntimeError("admin world drain clear_fail metric changed after successful DELETE")

        world_transfer_path = f"/api/v1/worlds/{world_scope.get('world_id')}/transfer"
        metrics_before_transfer = load_text("/metrics")
        world_transfer_requests_before = parse_prometheus_counter(
            metrics_before_transfer, "admin_world_transfer_requests_total"
        )
        world_transfer_write_before = parse_prometheus_counter(
            metrics_before_transfer, "admin_world_transfer_write_total"
        )
        world_transfer_write_fail_before = parse_prometheus_counter(
            metrics_before_transfer, "admin_world_transfer_write_fail_total"
        )
        world_transfer_clear_before = parse_prometheus_counter(
            metrics_before_transfer, "admin_world_transfer_clear_total"
        )
        world_transfer_clear_fail_before = parse_prometheus_counter(
            metrics_before_transfer, "admin_world_transfer_clear_fail_total"
        )
        world_transfer_owner_commit_before = parse_prometheus_counter(
            metrics_before_transfer, "admin_world_transfer_owner_commit_total"
        )
        world_transfer_owner_commit_fail_before = parse_prometheus_counter(
            metrics_before_transfer, "admin_world_transfer_owner_commit_fail_total"
        )

        transfer_initial = load_json(world_transfer_path)
        transfer_initial_data = transfer_initial.get("data", {})
        if transfer_initial_data.get("transfer", {}).get("phase") != "idle":
            raise RuntimeError("world transfer GET should report idle before PUT")
        if transfer_initial_data.get("owner_commit_applied") is not False:
            raise RuntimeError("world transfer GET should report owner_commit_applied=false")
        if int(transfer_initial_data.get("continuity_lease_ttl_sec") or 0) < 30:
            raise RuntimeError("world transfer GET continuity_lease_ttl_sec mismatch")

        status, _, payload = request_json_body(
            world_transfer_path,
            method="PUT",
            body_obj={
                "target_owner_instance_id": replacement_owner,
                "expected_owner_instance_id": world_scope.get("owner_instance_id"),
                "commit_owner": True,
            },
        )
        if status != 200:
            raise RuntimeError(f"world transfer PUT expected 200, got {status}")
        transfer_response = (payload or {}).get("data", {})
        if transfer_response.get("owner_instance_id") != replacement_owner:
            raise RuntimeError("world transfer PUT owner_instance_id mismatch")
        if transfer_response.get("owner_commit_applied") is not True:
            raise RuntimeError("world transfer PUT owner_commit_applied mismatch")
        transfer_policy = transfer_response.get("policy", {})
        if transfer_policy.get("draining") is not True:
            raise RuntimeError("world transfer PUT policy.draining mismatch")
        if transfer_policy.get("replacement_owner_instance_id") != replacement_owner:
            raise RuntimeError("world transfer PUT replacement_owner_instance_id mismatch")
        transfer_state = transfer_response.get("transfer", {})
        if transfer_state.get("phase") != "owner_handoff_committed":
            raise RuntimeError("world transfer PUT phase mismatch")
        transfer_summary = transfer_state.get("summary", {})
        if transfer_summary.get("owner_matches_target") is not True:
            raise RuntimeError("world transfer PUT owner_matches_target mismatch")

        metrics_after_transfer_put = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_requests_total"
            )
            <= world_transfer_requests_before
        ):
            raise RuntimeError("admin world transfer requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_write_total"
            )
            <= world_transfer_write_before
        ):
            raise RuntimeError("admin world transfer write metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_owner_commit_total"
            )
            <= world_transfer_owner_commit_before
        ):
            raise RuntimeError("admin world transfer owner_commit metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_write_fail_total"
            )
            != world_transfer_write_fail_before
        ):
            raise RuntimeError("admin world transfer write_fail metric changed after successful PUT")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_owner_commit_fail_total"
            )
            != world_transfer_owner_commit_fail_before
        ):
            raise RuntimeError("admin world transfer owner_commit_fail metric changed after successful PUT")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_clear_total"
            )
            != world_transfer_clear_before
        ):
            raise RuntimeError("admin world transfer clear metric changed before DELETE")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_clear_fail_total"
            )
            != world_transfer_clear_fail_before
        ):
            raise RuntimeError("admin world transfer clear_fail metric changed before DELETE")

        transferred_worlds = load_json("/api/v1/worlds?limit=100")
        transferred_world_item = next(
            (
                item
                for item in transferred_worlds.get("data", {}).get("items", [])
                if item.get("world_id") == world_scope.get("world_id")
            ),
            None,
        )
        if not isinstance(transferred_world_item, dict):
            raise RuntimeError("world inventory item missing after world transfer PUT")
        if transferred_world_item.get("owner_instance_id") != replacement_owner:
            raise RuntimeError("world inventory owner_instance_id did not move after transfer PUT")
        if transferred_world_item.get("transfer", {}).get("phase") != "owner_handoff_committed":
            raise RuntimeError("world inventory transfer phase mismatch after transfer PUT")

        status, _, payload = request_json_body(
            world_transfer_path,
            method="PUT",
            body_obj={
                "target_owner_instance_id": replacement_owner,
                "expected_owner_instance_id": "not-the-current-owner",
                "commit_owner": False,
            },
        )
        if status != 409:
            raise RuntimeError(f"world transfer OWNER_MISMATCH expected 409, got {status}")
        if (payload or {}).get("error", {}).get("code") != "OWNER_MISMATCH":
            raise RuntimeError("world transfer expected OWNER_MISMATCH")

        status, _, payload = request_json_body(
            world_transfer_path,
            method="PUT",
            body_obj={
                "target_owner_instance_id": "not-a-real-instance",
                "commit_owner": False,
            },
        )
        if status != 400:
            raise RuntimeError(f"world transfer invalid target expected 400, got {status}")
        if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
            raise RuntimeError("world transfer invalid target expected BAD_REQUEST")

        metrics_after_transfer_invalid = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_invalid, "admin_world_transfer_write_total"
            )
            != parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_write_total"
            )
        ):
            raise RuntimeError("admin world transfer write metric changed after invalid PUTs")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_invalid, "admin_world_transfer_owner_commit_total"
            )
            != parse_prometheus_counter(
                metrics_after_transfer_put, "admin_world_transfer_owner_commit_total"
            )
        ):
            raise RuntimeError("admin world transfer owner_commit metric changed after invalid PUTs")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_invalid, "admin_world_transfer_write_fail_total"
            )
            < world_transfer_write_fail_before + 2
        ):
            raise RuntimeError("admin world transfer write_fail metric did not increase after invalid PUTs")

        status, _, payload = request_json(world_transfer_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"world transfer DELETE expected 200, got {status}")
        transfer_cleared = (payload or {}).get("data", {})
        if transfer_cleared.get("owner_instance_id") != replacement_owner:
            raise RuntimeError("world transfer DELETE should preserve committed owner")
        if transfer_cleared.get("policy", {}).get("draining") is not False:
            raise RuntimeError("world transfer DELETE should clear draining policy")
        if transfer_cleared.get("transfer", {}).get("phase") != "idle":
            raise RuntimeError("world transfer DELETE should reset phase to idle")

        metrics_after_transfer_delete = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_delete, "admin_world_transfer_clear_total"
            )
            <= world_transfer_clear_before
        ):
            raise RuntimeError("admin world transfer clear metric did not increase after DELETE")
        if (
            parse_prometheus_counter(
                metrics_after_transfer_delete, "admin_world_transfer_clear_fail_total"
            )
            != world_transfer_clear_fail_before
        ):
            raise RuntimeError("admin world transfer clear_fail metric changed after successful DELETE")

        transfer_cleared_worlds = load_json("/api/v1/worlds?limit=100")
        transfer_cleared_world_item = next(
            (
                item
                for item in transfer_cleared_worlds.get("data", {}).get("items", [])
                if item.get("world_id") == world_scope.get("world_id")
            ),
            None,
        )
        if not isinstance(transfer_cleared_world_item, dict):
            raise RuntimeError("world inventory item missing after world transfer DELETE")
        if transfer_cleared_world_item.get("owner_instance_id") != replacement_owner:
            raise RuntimeError("world inventory owner_instance_id should stay committed after transfer DELETE")
        if transfer_cleared_world_item.get("policy", {}).get("draining") is not False:
            raise RuntimeError("world inventory policy.draining should clear after world transfer DELETE")
        if transfer_cleared_world_item.get("transfer", {}).get("phase") != "idle":
            raise RuntimeError("world inventory transfer phase should reset after world transfer DELETE")

        migration_target = first_server_for_other_world(
            STACK_TOPOLOGY,
            str(world_scope.get("world_id")),
        )
        if migration_target is None:
            raise RuntimeError("active topology does not define a migration target world")
        world_migration_path = f"/api/v1/worlds/{world_scope.get('world_id')}/migration"
        metrics_before_world_migration = load_text("/metrics")
        world_migration_requests_before = parse_prometheus_counter(
            metrics_before_world_migration, "admin_world_migration_requests_total"
        )
        world_migration_write_before = parse_prometheus_counter(
            metrics_before_world_migration, "admin_world_migration_write_total"
        )
        world_migration_write_fail_before = parse_prometheus_counter(
            metrics_before_world_migration, "admin_world_migration_write_fail_total"
        )
        world_migration_clear_before = parse_prometheus_counter(
            metrics_before_world_migration, "admin_world_migration_clear_total"
        )
        world_migration_clear_fail_before = parse_prometheus_counter(
            metrics_before_world_migration, "admin_world_migration_clear_fail_total"
        )

        migration_initial = load_json(world_migration_path)
        migration_initial_data = migration_initial.get("data", {})
        migration_initial_state = migration_initial_data.get("migration", {})
        if migration_initial_state.get("phase") != "idle":
            raise RuntimeError("world migration GET should report idle before PUT")

        status, _, payload = request_json_body(
            world_migration_path,
            method="PUT",
            body_obj={
                "target_world_id": str(migration_target["world_id"]),
                "target_owner_instance_id": str(migration_target["instance_id"]),
                "preserve_room": True,
                "payload_kind": "chat-room-v1",
                "payload_ref": "migration-target-room",
            },
        )
        if status != 200:
            raise RuntimeError(f"world migration PUT expected 200, got {status}")
        migration_response = (payload or {}).get("data", {})
        migration_state = migration_response.get("migration", {})
        if migration_state.get("phase") != "awaiting_source_drain":
            raise RuntimeError("world migration PUT phase mismatch")
        if migration_state.get("target_world_id") != str(migration_target["world_id"]):
            raise RuntimeError("world migration PUT target_world_id mismatch")
        if migration_state.get("target_owner_instance_id") != str(migration_target["instance_id"]):
            raise RuntimeError("world migration PUT target_owner_instance_id mismatch")
        if migration_state.get("payload_kind") != "chat-room-v1":
            raise RuntimeError("world migration PUT payload_kind mismatch")
        if migration_state.get("payload_ref") != "migration-target-room":
            raise RuntimeError("world migration PUT payload_ref mismatch")
        if migration_state.get("summary", {}).get("preserve_room") is not True:
            raise RuntimeError("world migration PUT preserve_room mismatch")

        metrics_after_world_migration_put = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_put, "admin_world_migration_requests_total"
            )
            <= world_migration_requests_before
        ):
            raise RuntimeError("admin world migration requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_put, "admin_world_migration_write_total"
            )
            <= world_migration_write_before
        ):
            raise RuntimeError("admin world migration write metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_put, "admin_world_migration_write_fail_total"
            )
            != world_migration_write_fail_before
        ):
            raise RuntimeError("admin world migration write_fail metric changed after successful PUT")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_put, "admin_world_migration_clear_total"
            )
            != world_migration_clear_before
        ):
            raise RuntimeError("admin world migration clear metric changed before DELETE")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_put, "admin_world_migration_clear_fail_total"
            )
            != world_migration_clear_fail_before
        ):
            raise RuntimeError("admin world migration clear_fail metric changed before DELETE")

        migrated_worlds = load_json("/api/v1/worlds?limit=100")
        migrated_world_item = next(
            (
                item
                for item in migrated_worlds.get("data", {}).get("items", [])
                if item.get("world_id") == world_scope.get("world_id")
            ),
            None,
        )
        if not isinstance(migrated_world_item, dict):
            raise RuntimeError("world inventory item missing after world migration PUT")
        if migrated_world_item.get("migration", {}).get("phase") != "awaiting_source_drain":
            raise RuntimeError("world inventory migration phase mismatch after PUT")

        status, _, payload = request_json_body(
            world_migration_path,
            method="PUT",
            body_obj={
                "target_world_id": str(world_scope.get("world_id")),
                "target_owner_instance_id": str(migration_target["instance_id"]),
            },
        )
        if status != 400:
            raise RuntimeError(f"world migration same-target-world expected 400, got {status}")
        if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
            raise RuntimeError("world migration same-target-world expected BAD_REQUEST")

        status, _, payload = request_json_body(
            world_migration_path,
            method="PUT",
            body_obj={
                "target_world_id": str(migration_target["world_id"]),
                "target_owner_instance_id": "not-a-real-instance",
            },
        )
        if status != 400:
            raise RuntimeError(f"world migration invalid target owner expected 400, got {status}")
        if (payload or {}).get("error", {}).get("code") != "BAD_REQUEST":
            raise RuntimeError("world migration invalid target owner expected BAD_REQUEST")

        metrics_after_world_migration_invalid = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_invalid, "admin_world_migration_write_total"
            )
            != parse_prometheus_counter(
                metrics_after_world_migration_put, "admin_world_migration_write_total"
            )
        ):
            raise RuntimeError("admin world migration write metric changed after invalid PUTs")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_invalid, "admin_world_migration_write_fail_total"
            )
            < world_migration_write_fail_before + 2
        ):
            raise RuntimeError("admin world migration write_fail metric did not increase after invalid PUTs")

        status, _, payload = request_json(world_migration_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"world migration DELETE expected 200, got {status}")
        migration_cleared = (payload or {}).get("data", {})
        if migration_cleared.get("migration", {}).get("phase") != "idle":
            raise RuntimeError("world migration DELETE should reset phase to idle")

        metrics_after_world_migration_delete = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_delete, "admin_world_migration_clear_total"
            )
            <= world_migration_clear_before
        ):
            raise RuntimeError("admin world migration clear metric did not increase after DELETE")
        if (
            parse_prometheus_counter(
                metrics_after_world_migration_delete, "admin_world_migration_clear_fail_total"
            )
            != world_migration_clear_fail_before
        ):
            raise RuntimeError("admin world migration clear_fail metric changed after successful DELETE")

        desired_topology_path = "/api/v1/topology/desired"
        metrics_before_topology = load_text("/metrics")
        desired_topology_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_desired_topology_requests_total"
        )
        desired_topology_write_before = parse_prometheus_counter(
            metrics_before_topology, "admin_desired_topology_write_total"
        )
        desired_topology_write_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_desired_topology_write_fail_total"
        )
        desired_topology_clear_before = parse_prometheus_counter(
            metrics_before_topology, "admin_desired_topology_clear_total"
        )
        desired_topology_clear_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_desired_topology_clear_fail_total"
        )
        observed_topology_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_observed_topology_requests_total"
        )
        topology_actuation_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_requests_total"
        )
        topology_actuation_request_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_request_requests_total"
        )
        topology_actuation_request_write_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_request_write_total"
        )
        topology_actuation_request_write_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_request_write_fail_total"
        )
        topology_actuation_request_clear_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_request_clear_total"
        )
        topology_actuation_request_clear_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_request_clear_fail_total"
        )
        topology_actuation_status_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_status_requests_total"
        )
        topology_actuation_execution_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_execution_requests_total"
        )
        topology_actuation_execution_write_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_execution_write_total"
        )
        topology_actuation_execution_write_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_execution_write_fail_total"
        )
        topology_actuation_execution_clear_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_execution_clear_total"
        )
        topology_actuation_execution_clear_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_execution_clear_fail_total"
        )
        topology_actuation_execution_status_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_execution_status_requests_total"
        )
        topology_actuation_realization_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_realization_requests_total"
        )
        topology_actuation_adapter_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_adapter_requests_total"
        )
        topology_actuation_adapter_write_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_adapter_write_total"
        )
        topology_actuation_adapter_write_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_adapter_write_fail_total"
        )
        topology_actuation_adapter_clear_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_adapter_clear_total"
        )
        topology_actuation_adapter_clear_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_adapter_clear_fail_total"
        )
        topology_actuation_adapter_status_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_adapter_status_requests_total"
        )
        topology_actuation_runtime_assignment_requests_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_runtime_assignment_requests_total"
        )
        topology_actuation_runtime_assignment_write_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_runtime_assignment_write_total"
        )
        topology_actuation_runtime_assignment_write_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_runtime_assignment_write_fail_total"
        )
        topology_actuation_runtime_assignment_clear_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_runtime_assignment_clear_total"
        )
        topology_actuation_runtime_assignment_clear_fail_before = parse_prometheus_counter(
            metrics_before_topology, "admin_topology_actuation_runtime_assignment_clear_fail_total"
        )

        observed_topology = load_json("/api/v1/topology/observed?timeout_ms=5000")
        observed_data = observed_topology.get("data", {})
        observed_instances = observed_data.get("instances", [])
        observed_worlds = observed_data.get("worlds", [])
        observed_summary = observed_data.get("summary", {})
        if not isinstance(observed_instances, list) or not observed_instances:
            raise RuntimeError("observed topology missing instances")
        if not isinstance(observed_worlds, list) or not observed_worlds:
            raise RuntimeError("observed topology missing worlds")
        if observed_summary.get("instances_total") != len(observed_instances):
            raise RuntimeError("observed topology instances_total mismatch")
        if observed_summary.get("worlds_total") != len(observed_worlds):
            raise RuntimeError("observed topology worlds_total mismatch")
        observed_world_entry = next(
            (
                item
                for item in observed_worlds
                if item.get("world_id") == world_scope.get("world_id")
            ),
            None,
        )
        if not isinstance(observed_world_entry, dict):
            raise RuntimeError("observed topology missing current world entry")
        observed_instance_entry = next(
            (
                item
                for item in observed_instances
                if item.get("instance_id") == world_server["instance_id"]
            ),
            None,
        )
        if not isinstance(observed_instance_entry, dict):
            raise RuntimeError("observed topology missing current instance entry")
        if not isinstance(observed_instance_entry.get("world_scope"), dict):
            raise RuntimeError("observed topology instance missing world_scope")

        for cleanup_path, cleanup_label in (
            ("/api/v1/topology/actuation/runtime-assignment", "topology actuation runtime assignment"),
            ("/api/v1/topology/actuation/adapter", "topology actuation adapter"),
            ("/api/v1/topology/actuation/execution", "topology actuation execution"),
            ("/api/v1/topology/actuation/request", "topology actuation request"),
            (desired_topology_path, "desired topology"),
        ):
            status, _, _ = request_json(cleanup_path, method="DELETE")
            if status != 200:
                raise RuntimeError(f"{cleanup_label} cleanup DELETE expected 200, got {status}")

        actuation_topology_path = "/api/v1/topology/actuation"
        actuation_initial = load_json(actuation_topology_path)
        actuation_initial_data = actuation_initial.get("data", {})
        if actuation_initial_data.get("desired") is not None:
            raise RuntimeError("topology actuation should report null desired before first PUT")
        if actuation_initial_data.get("summary", {}).get("desired_present") is not False:
            raise RuntimeError("topology actuation should report desired_present=false before first PUT")
        initial_observed_pools = actuation_initial_data.get("observed_pools", [])
        initial_actions = actuation_initial_data.get("actions", [])
        initial_summary = actuation_initial_data.get("summary", {})
        if initial_summary.get("actions_total") != len(initial_actions):
            raise RuntimeError("topology actuation initial actions_total should match actions length")
        if initial_summary.get("actions_total") != len(initial_observed_pools):
            raise RuntimeError("topology actuation should surface one observe-only action per observed pool before first PUT")
        if initial_summary.get("actionable_actions") != 0:
            raise RuntimeError("topology actuation should not report actionable actions before first PUT")
        if initial_summary.get("observe_only_actions") != len(initial_actions):
            raise RuntimeError("topology actuation should classify all initial actions as observe-only")
        for action in initial_actions:
            if action.get("action") != "observe_undeclared_pool":
                raise RuntimeError("topology actuation initial action should be observe_undeclared_pool")
            if action.get("actionable") is not False:
                raise RuntimeError("topology actuation initial observe-only action should not be actionable")

        actuation_request_path = "/api/v1/topology/actuation/request"
        actuation_status_path = "/api/v1/topology/actuation/status"
        actuation_execution_path = "/api/v1/topology/actuation/execution"
        actuation_execution_status_path = "/api/v1/topology/actuation/execution/status"
        actuation_realization_path = "/api/v1/topology/actuation/realization"
        actuation_adapter_path = "/api/v1/topology/actuation/adapter"
        actuation_adapter_status_path = "/api/v1/topology/actuation/adapter/status"
        actuation_runtime_assignment_path = "/api/v1/topology/actuation/runtime-assignment"

        actuation_request_initial = load_json(actuation_request_path)
        actuation_request_initial_data = actuation_request_initial.get("data", {})
        if actuation_request_initial_data.get("present") is not False:
            raise RuntimeError("topology actuation request should be absent before first PUT")
        if actuation_request_initial_data.get("request") is not None:
            raise RuntimeError("topology actuation request should return null request before first PUT")

        actuation_status_initial = load_json(actuation_status_path)
        actuation_status_initial_data = actuation_status_initial.get("data", {})
        if actuation_status_initial_data.get("request") is not None:
            raise RuntimeError("topology actuation status should report null request before first PUT")
        if actuation_status_initial_data.get("summary", {}).get("request_present") is not False:
            raise RuntimeError("topology actuation status should report request_present=false before first PUT")
        if actuation_status_initial_data.get("summary", {}).get("actions_total") != 0:
            raise RuntimeError("topology actuation status should report zero request actions before first PUT")

        actuation_execution_initial = load_json(actuation_execution_path)
        actuation_execution_initial_data = actuation_execution_initial.get("data", {})
        if actuation_execution_initial_data.get("present") is not False:
            raise RuntimeError("topology actuation execution should be absent before first PUT")
        if actuation_execution_initial_data.get("execution") is not None:
            raise RuntimeError("topology actuation execution should return null execution before first PUT")

        actuation_execution_status_initial = load_json(actuation_execution_status_path)
        actuation_execution_status_initial_data = actuation_execution_status_initial.get("data", {})
        if actuation_execution_status_initial_data.get("execution") is not None:
            raise RuntimeError("topology actuation execution status should report null execution before first PUT")
        if actuation_execution_status_initial_data.get("summary", {}).get("execution_present") is not False:
            raise RuntimeError("topology actuation execution status should report execution_present=false before first PUT")
        if actuation_execution_status_initial_data.get("summary", {}).get("actions_total") != 0:
            raise RuntimeError("topology actuation execution status should report zero actions before first PUT")

        actuation_realization_initial = load_json(actuation_realization_path)
        actuation_realization_initial_data = actuation_realization_initial.get("data", {})
        if actuation_realization_initial_data.get("execution") is not None:
            raise RuntimeError("topology actuation realization should report null execution before first PUT")
        if actuation_realization_initial_data.get("summary", {}).get("actions_total") != 0:
            raise RuntimeError("topology actuation realization should report zero actions before first PUT")

        actuation_adapter_initial = load_json(actuation_adapter_path)
        actuation_adapter_initial_data = actuation_adapter_initial.get("data", {})
        if actuation_adapter_initial_data.get("present") is not False:
            raise RuntimeError("topology actuation adapter should be absent before first PUT")
        if actuation_adapter_initial_data.get("lease") is not None:
            raise RuntimeError("topology actuation adapter should return null lease before first PUT")

        actuation_adapter_status_initial = load_json(actuation_adapter_status_path)
        actuation_adapter_status_initial_data = actuation_adapter_status_initial.get("data", {})
        if actuation_adapter_status_initial_data.get("lease") is not None:
            raise RuntimeError("topology actuation adapter status should report null lease before first PUT")
        if actuation_adapter_status_initial_data.get("summary", {}).get("actions_total") != 0:
            raise RuntimeError("topology actuation adapter status should report zero actions before first PUT")

        actuation_runtime_assignment_initial = load_json(actuation_runtime_assignment_path)
        actuation_runtime_assignment_initial_data = actuation_runtime_assignment_initial.get("data", {})
        if actuation_runtime_assignment_initial_data.get("present") is not False:
            raise RuntimeError("topology actuation runtime assignment should be absent before first PUT")
        if actuation_runtime_assignment_initial_data.get("assignment") is not None:
            raise RuntimeError("topology actuation runtime assignment should return null assignment before first PUT")

        desired_initial = load_json(desired_topology_path)
        desired_initial_data = desired_initial.get("data", {})
        if desired_initial_data.get("present") is not False:
            raise RuntimeError("desired topology should be absent before first PUT")
        if desired_initial_data.get("topology") is not None:
            raise RuntimeError("desired topology initial topology should be null")

        desired_body_v1 = {
            "topology_id": "starter-topology",
            "pools": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "replicas": 2,
                    "capacity_class": "medium",
                    "placement_tags": ["region:dev", "tier:starter"],
                }
            ],
        }
        status, _, payload = request_json_body(
            desired_topology_path,
            method="PUT",
            body_obj=desired_body_v1,
        )
        if status != 200:
            raise RuntimeError(f"desired topology PUT v1 expected 200, got {status}")
        desired_response_v1 = (payload or {}).get("data", {})
        topology_v1 = desired_response_v1.get("topology", {})
        if desired_response_v1.get("present") is not True:
            raise RuntimeError("desired topology PUT v1 did not mark document present")
        if topology_v1.get("revision") != 1:
            raise RuntimeError("desired topology PUT v1 revision mismatch")
        if topology_v1.get("topology_id") != "starter-topology":
            raise RuntimeError("desired topology PUT v1 topology_id mismatch")

        desired_body_v2 = {
            "topology_id": "starter-topology",
            "expected_revision": 1,
            "pools": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "replicas": 3,
                    "capacity_class": "large",
                    "placement_tags": ["region:dev", "tier:starter"],
                }
            ],
        }
        status, _, payload = request_json_body(
            desired_topology_path,
            method="PUT",
            body_obj=desired_body_v2,
        )
        if status != 200:
            raise RuntimeError(f"desired topology PUT v2 expected 200, got {status}")
        desired_response_v2 = (payload or {}).get("data", {})
        topology_v2 = desired_response_v2.get("topology", {})
        if topology_v2.get("revision") != 2:
            raise RuntimeError("desired topology PUT v2 revision mismatch")
        pools_v2 = topology_v2.get("pools", [])
        if not isinstance(pools_v2, list) or not pools_v2 or pools_v2[0].get("replicas") != 3:
            raise RuntimeError("desired topology PUT v2 pools mismatch")

        status, _, payload = request_json_body(
            desired_topology_path,
            method="PUT",
            body_obj={
                "topology_id": "starter-topology",
                "expected_revision": 1,
                "pools": desired_body_v2["pools"],
            },
        )
        if status != 409:
            raise RuntimeError(f"desired topology stale PUT expected 409, got {status}")
        if (payload or {}).get("error", {}).get("code") != "REVISION_MISMATCH":
            raise RuntimeError("desired topology stale PUT expected REVISION_MISMATCH")

        desired_after_put = load_json(desired_topology_path)
        desired_after_put_data = desired_after_put.get("data", {})
        if desired_after_put_data.get("present") is not True:
            raise RuntimeError("desired topology GET after PUT should be present")
        if desired_after_put_data.get("topology", {}).get("revision") != 2:
            raise RuntimeError("desired topology GET after PUT revision mismatch")

        actuation_after_put = load_json(f"{actuation_topology_path}?timeout_ms=5000")
        actuation_after_put_data = actuation_after_put.get("data", {})
        actuation_summary = actuation_after_put_data.get("summary", {})
        if actuation_summary.get("desired_present") is not True:
            raise RuntimeError("topology actuation should report desired_present=true after PUT")
        if actuation_summary.get("actions_total") != 2:
            raise RuntimeError("topology actuation actions_total mismatch after PUT")
        if actuation_summary.get("actionable_actions") != 1:
            raise RuntimeError("topology actuation actionable_actions mismatch after PUT")
        if actuation_summary.get("scale_out_actions") != 1:
            raise RuntimeError("topology actuation scale_out_actions mismatch after PUT")
        if actuation_summary.get("observe_only_actions") != 1:
            raise RuntimeError("topology actuation observe_only_actions mismatch after PUT")

        actuation_actions = actuation_after_put_data.get("actions", [])
        if not isinstance(actuation_actions, list) or len(actuation_actions) != 2:
            raise RuntimeError("topology actuation actions payload mismatch after PUT")
        scale_out_action = next(
            (
                item
                for item in actuation_actions
                if item.get("world_id") == str(world_scope.get("world_id"))
            ),
            None,
        )
        if not isinstance(scale_out_action, dict):
            raise RuntimeError("topology actuation missing current world action after PUT")
        if scale_out_action.get("action") != "scale_out_pool":
            raise RuntimeError("topology actuation current world action mismatch after PUT")
        if scale_out_action.get("replica_delta") != 2:
            raise RuntimeError("topology actuation current world replica_delta mismatch after PUT")
        if scale_out_action.get("actionable") is not True:
            raise RuntimeError("topology actuation current world actionable mismatch after PUT")

        observe_only_action = next(
            (
                item
                for item in actuation_actions
                if item.get("world_id") != str(world_scope.get("world_id"))
            ),
            None,
        )
        if not isinstance(observe_only_action, dict):
            raise RuntimeError("topology actuation missing undeclared observed pool action after PUT")
        if observe_only_action.get("action") != "observe_undeclared_pool":
            raise RuntimeError("topology actuation undeclared pool action mismatch after PUT")
        if observe_only_action.get("actionable") is not False:
            raise RuntimeError("topology actuation undeclared pool actionable mismatch after PUT")

        actuation_request_body_v1 = {
            "request_id": "starter-scale-request",
            "basis_topology_revision": 2,
            "actions": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "action": "scale_out_pool",
                    "replica_delta": 2,
                }
            ],
        }
        status, _, payload = request_json_body(
            actuation_request_path,
            method="PUT",
            body_obj=actuation_request_body_v1,
        )
        if status != 200:
            raise RuntimeError(f"topology actuation request PUT v1 expected 200, got {status}")
        actuation_request_response_v1 = (payload or {}).get("data", {})
        actuation_request_v1 = actuation_request_response_v1.get("request", {})
        if actuation_request_response_v1.get("present") is not True:
            raise RuntimeError("topology actuation request PUT v1 should mark request present")
        if actuation_request_v1.get("revision") != 1:
            raise RuntimeError("topology actuation request PUT v1 revision mismatch")
        if actuation_request_v1.get("basis_topology_revision") != 2:
            raise RuntimeError("topology actuation request PUT v1 basis revision mismatch")

        status, _, payload = request_json_body(
            actuation_request_path,
            method="PUT",
            body_obj={
                **actuation_request_body_v1,
                "expected_revision": 0,
            },
        )
        if status != 409:
            raise RuntimeError(f"topology actuation request stale PUT expected 409, got {status}")
        if (payload or {}).get("error", {}).get("code") != "REVISION_MISMATCH":
            raise RuntimeError("topology actuation request stale PUT expected REVISION_MISMATCH")

        actuation_status_pending = load_json(f"{actuation_status_path}?timeout_ms=5000")
        actuation_status_pending_data = actuation_status_pending.get("data", {})
        actuation_status_pending_summary = actuation_status_pending_data.get("summary", {})
        if actuation_status_pending_summary.get("request_present") is not True:
            raise RuntimeError("topology actuation status should report request_present=true after request PUT")
        if actuation_status_pending_summary.get("pending_actions") != 1:
            raise RuntimeError("topology actuation status pending_actions mismatch after request PUT")
        if actuation_status_pending_summary.get("superseded_actions") != 0:
            raise RuntimeError("topology actuation status superseded_actions mismatch after request PUT")
        if actuation_status_pending_summary.get("basis_topology_revision_matches_current") is not True:
            raise RuntimeError("topology actuation status should report basis revision match after request PUT")
        pending_actions = actuation_status_pending_data.get("actions", [])
        if not isinstance(pending_actions, list) or len(pending_actions) != 1:
            raise RuntimeError("topology actuation status pending actions payload mismatch")
        pending_action = pending_actions[0]
        if pending_action.get("state") != "pending":
            raise RuntimeError("topology actuation status pending action state mismatch")
        if pending_action.get("current_action") != "scale_out_pool":
            raise RuntimeError("topology actuation status pending current_action mismatch")
        if pending_action.get("current_replica_delta") != 2:
            raise RuntimeError("topology actuation status pending current_replica_delta mismatch")

        actuation_execution_available = load_json(f"{actuation_execution_status_path}?timeout_ms=5000")
        actuation_execution_available_data = actuation_execution_available.get("data", {})
        actuation_execution_available_summary = actuation_execution_available_data.get("summary", {})
        if actuation_execution_available_summary.get("available_actions") != 1:
            raise RuntimeError("topology actuation execution status available_actions mismatch before execution PUT")
        actuation_realization_available = load_json(f"{actuation_realization_path}?timeout_ms=5000")
        if actuation_realization_available.get("data", {}).get("summary", {}).get("available_actions") != 1:
            raise RuntimeError("topology actuation realization available_actions mismatch before execution PUT")

        actuation_execution_body_v1 = {
            "executor_id": "local-executor",
            "request_revision": 1,
            "actions": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "action": "scale_out_pool",
                    "replica_delta": 2,
                    "observed_instances_before": 1,
                    "ready_instances_before": 1,
                    "state": "claimed",
                }
            ],
        }
        status, _, payload = request_json_body(
            actuation_execution_path,
            method="PUT",
            body_obj=actuation_execution_body_v1,
        )
        if status != 200:
            raise RuntimeError(f"topology actuation execution PUT v1 expected 200, got {status}")
        execution_response_v1 = (payload or {}).get("data", {})
        execution_v1 = execution_response_v1.get("execution", {})
        if execution_response_v1.get("present") is not True:
            raise RuntimeError("topology actuation execution PUT v1 should mark execution present")
        if execution_v1.get("revision") != 1:
            raise RuntimeError("topology actuation execution PUT v1 revision mismatch")

        actuation_execution_status_claimed = load_json(f"{actuation_execution_status_path}?timeout_ms=5000")
        actuation_execution_status_claimed_data = actuation_execution_status_claimed.get("data", {})
        actuation_execution_status_claimed_summary = actuation_execution_status_claimed_data.get("summary", {})
        if actuation_execution_status_claimed_summary.get("claimed_actions") != 1:
            raise RuntimeError("topology actuation execution status claimed_actions mismatch after claimed PUT")
        actuation_realization_claimed = load_json(f"{actuation_realization_path}?timeout_ms=5000")
        actuation_realization_claimed_data = actuation_realization_claimed.get("data", {})
        if actuation_realization_claimed_data.get("summary", {}).get("claimed_actions") != 1:
            raise RuntimeError("topology actuation realization claimed_actions mismatch after claimed PUT")

        actuation_adapter_available = load_json(f"{actuation_adapter_status_path}?timeout_ms=5000")
        if actuation_adapter_available.get("data", {}).get("summary", {}).get("available_actions") != 1:
            raise RuntimeError("topology actuation adapter status available_actions mismatch before adapter PUT")

        actuation_adapter_body_v1 = {
            "adapter_id": "local-adapter",
            "execution_revision": 1,
            "actions": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "action": "scale_out_pool",
                    "replica_delta": 2,
                }
            ],
        }
        status, _, payload = request_json_body(
            actuation_adapter_path,
            method="PUT",
            body_obj=actuation_adapter_body_v1,
        )
        if status != 200:
            raise RuntimeError(f"topology actuation adapter PUT v1 expected 200, got {status}")
        adapter_response_v1 = (payload or {}).get("data", {})
        adapter_v1 = adapter_response_v1.get("lease", {})
        if adapter_response_v1.get("present") is not True:
            raise RuntimeError("topology actuation adapter PUT v1 should mark lease present")
        if adapter_v1.get("revision") != 1:
            raise RuntimeError("topology actuation adapter PUT v1 revision mismatch")
        if adapter_v1.get("execution_revision") != 1:
            raise RuntimeError("topology actuation adapter PUT v1 execution revision mismatch")

        actuation_adapter_status_leased = load_json(f"{actuation_adapter_status_path}?timeout_ms=5000")
        if actuation_adapter_status_leased.get("data", {}).get("summary", {}).get("leased_actions") != 1:
            raise RuntimeError("topology actuation adapter status leased_actions mismatch after adapter PUT")

        status, _, payload = request_json_body(
            actuation_execution_path,
            method="PUT",
            body_obj={
                **actuation_execution_body_v1,
                "expected_revision": 1,
                "actions": [
                    {
                        "world_id": str(world_scope.get("world_id")),
                        "shard": str(world_server.get("shard")),
                        "action": "scale_out_pool",
                        "replica_delta": 2,
                        "observed_instances_before": 1,
                        "ready_instances_before": 1,
                        "state": "failed",
                    }
                ],
            },
        )
        if status != 200:
            raise RuntimeError(f"topology actuation execution PUT v2 expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("execution", {}) or {}).get("revision") != 2:
            raise RuntimeError("topology actuation execution PUT v2 revision mismatch")

        actuation_execution_status_failed = load_json(f"{actuation_execution_status_path}?timeout_ms=5000")
        actuation_execution_status_failed_data = actuation_execution_status_failed.get("data", {})
        if actuation_execution_status_failed_data.get("summary", {}).get("failed_actions") != 1:
            raise RuntimeError("topology actuation execution status failed_actions mismatch after failed PUT")
        actuation_realization_failed = load_json(f"{actuation_realization_path}?timeout_ms=5000")
        if actuation_realization_failed.get("data", {}).get("summary", {}).get("failed_actions") != 1:
            raise RuntimeError("topology actuation realization failed_actions mismatch after failed PUT")
        actuation_adapter_status_stale_after_failed = load_json(f"{actuation_adapter_status_path}?timeout_ms=5000")
        if actuation_adapter_status_stale_after_failed.get("data", {}).get("summary", {}).get("stale_actions") != 1:
            raise RuntimeError("topology actuation adapter status stale_actions mismatch after failed PUT")

        actuation_adapter_body_v2 = {
            **actuation_adapter_body_v1,
            "execution_revision": 2,
            "expected_revision": 1,
        }
        status, _, payload = request_json_body(
            actuation_adapter_path,
            method="PUT",
            body_obj=actuation_adapter_body_v2,
        )
        if status != 200:
            raise RuntimeError(f"topology actuation adapter PUT v2 expected 200, got {status}")
        adapter_response_v2 = (payload or {}).get("data", {})
        adapter_v2 = adapter_response_v2.get("lease", {})
        if adapter_response_v2.get("present") is not True:
            raise RuntimeError("topology actuation adapter PUT v2 should mark lease present")
        if adapter_v2.get("revision") != 2:
            raise RuntimeError("topology actuation adapter PUT v2 revision mismatch")
        if adapter_v2.get("execution_revision") != 2:
            raise RuntimeError("topology actuation adapter PUT v2 execution revision mismatch")

        actuation_adapter_status_failed = load_json(f"{actuation_adapter_status_path}?timeout_ms=5000")
        if actuation_adapter_status_failed.get("data", {}).get("summary", {}).get("failed_actions") != 1:
            raise RuntimeError("topology actuation adapter status failed_actions mismatch after adapter PUT v2")

        status, _, payload = request_json_body(
            actuation_execution_path,
            method="PUT",
            body_obj={
                **actuation_execution_body_v1,
                "expected_revision": 2,
                "actions": [
                    {
                        "world_id": str(world_scope.get("world_id")),
                        "shard": str(world_server.get("shard")),
                        "action": "scale_out_pool",
                        "replica_delta": 2,
                        "observed_instances_before": 1,
                        "ready_instances_before": 1,
                        "state": "completed",
                    }
                ],
            },
        )
        if status != 200:
            raise RuntimeError(f"topology actuation execution PUT v3 expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("execution", {}) or {}).get("revision") != 3:
            raise RuntimeError("topology actuation execution PUT v3 revision mismatch")

        actuation_execution_status_stale = load_json(f"{actuation_execution_status_path}?timeout_ms=5000")
        actuation_execution_status_stale_data = actuation_execution_status_stale.get("data", {})
        if actuation_execution_status_stale_data.get("summary", {}).get("stale_actions") != 1:
            raise RuntimeError("topology actuation execution status stale_actions mismatch after premature completed PUT")
        actuation_realization_stale = load_json(f"{actuation_realization_path}?timeout_ms=5000")
        if actuation_realization_stale.get("data", {}).get("summary", {}).get("stale_actions") != 1:
            raise RuntimeError("topology actuation realization stale_actions mismatch after premature completed PUT")
        actuation_adapter_status_stale = load_json(f"{actuation_adapter_status_path}?timeout_ms=5000")
        if actuation_adapter_status_stale.get("data", {}).get("summary", {}).get("stale_actions") != 1:
            raise RuntimeError("topology actuation adapter status stale_actions mismatch after premature completed PUT")

        status, _, payload = request_json_body(
            actuation_execution_path,
            method="PUT",
            body_obj={
                **actuation_execution_body_v1,
                "expected_revision": 0,
            },
        )
        if status != 409:
            raise RuntimeError(f"topology actuation execution stale PUT expected 409, got {status}")
        if (payload or {}).get("error", {}).get("code") != "REVISION_MISMATCH":
            raise RuntimeError("topology actuation execution stale PUT expected REVISION_MISMATCH")

        desired_body_v3 = {
            "topology_id": "starter-topology",
            "expected_revision": 2,
            "pools": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "replicas": 4,
                    "capacity_class": "large",
                    "placement_tags": ["region:dev", "tier:starter"],
                }
            ],
        }
        status, _, payload = request_json_body(
            desired_topology_path,
            method="PUT",
            body_obj=desired_body_v3,
        )
        if status != 200:
            raise RuntimeError(f"desired topology PUT v3 expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("topology", {}) or {}).get("revision") != 3:
            raise RuntimeError("desired topology PUT v3 revision mismatch")

        actuation_status_superseded = load_json(f"{actuation_status_path}?timeout_ms=5000")
        actuation_status_superseded_data = actuation_status_superseded.get("data", {})
        actuation_status_superseded_summary = actuation_status_superseded_data.get("summary", {})
        if actuation_status_superseded_summary.get("superseded_actions") != 1:
            raise RuntimeError("topology actuation status superseded_actions mismatch after desired PUT v3")
        if actuation_status_superseded_summary.get("basis_topology_revision_matches_current") is not False:
            raise RuntimeError("topology actuation status should report basis revision mismatch after desired PUT v3")
        superseded_actions = actuation_status_superseded_data.get("actions", [])
        if not isinstance(superseded_actions, list) or len(superseded_actions) != 1:
            raise RuntimeError("topology actuation status superseded actions payload mismatch")
        superseded_action = superseded_actions[0]
        if superseded_action.get("state") != "superseded":
            raise RuntimeError("topology actuation status superseded action state mismatch")
        if superseded_action.get("current_replica_delta") != 3:
            raise RuntimeError("topology actuation status superseded current_replica_delta mismatch")

        desired_body_v4 = {
            "topology_id": "starter-topology",
            "expected_revision": 3,
            "pools": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "replicas": 1,
                    "capacity_class": "medium",
                    "placement_tags": ["region:dev", "tier:starter"],
                }
            ],
        }
        status, _, payload = request_json_body(
            desired_topology_path,
            method="PUT",
            body_obj=desired_body_v4,
        )
        if status != 200:
            raise RuntimeError(f"desired topology PUT v4 expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("topology", {}) or {}).get("revision") != 4:
            raise RuntimeError("desired topology PUT v4 revision mismatch")

        actuation_status_satisfied = load_json(f"{actuation_status_path}?timeout_ms=5000")
        actuation_status_satisfied_data = actuation_status_satisfied.get("data", {})
        actuation_status_satisfied_summary = actuation_status_satisfied_data.get("summary", {})
        if actuation_status_satisfied_summary.get("satisfied_actions") != 1:
            raise RuntimeError("topology actuation status satisfied_actions mismatch after desired PUT v4")
        satisfied_actions = actuation_status_satisfied_data.get("actions", [])
        if not isinstance(satisfied_actions, list) or len(satisfied_actions) != 1:
            raise RuntimeError("topology actuation status satisfied actions payload mismatch")
        satisfied_action = satisfied_actions[0]
        if satisfied_action.get("state") != "satisfied":
            raise RuntimeError("topology actuation status satisfied action state mismatch")
        if satisfied_action.get("current_action") is not None:
            raise RuntimeError("topology actuation status satisfied current_action should be null")

        actuation_execution_status_completed = load_json(f"{actuation_execution_status_path}?timeout_ms=5000")
        actuation_execution_status_completed_data = actuation_execution_status_completed.get("data", {})
        if actuation_execution_status_completed_data.get("summary", {}).get("completed_actions") != 1:
            raise RuntimeError("topology actuation execution status completed_actions mismatch after desired PUT v4")
        actuation_realization_awaiting = load_json(f"{actuation_realization_path}?timeout_ms=5000")
        actuation_realization_awaiting_data = actuation_realization_awaiting.get("data", {})
        if actuation_realization_awaiting_data.get("summary", {}).get("awaiting_observation_actions") != 1:
            raise RuntimeError("topology actuation realization awaiting_observation_actions mismatch after desired PUT v4")
        awaiting_actions = actuation_realization_awaiting_data.get("actions", [])
        if not isinstance(awaiting_actions, list) or len(awaiting_actions) != 1:
            raise RuntimeError("topology actuation realization actions payload mismatch")
        awaiting_action = awaiting_actions[0]
        if awaiting_action.get("state") != "awaiting_observation":
            raise RuntimeError("topology actuation realization state mismatch after desired PUT v4")
        if awaiting_action.get("observed_instances_before") != 1:
            raise RuntimeError("topology actuation realization baseline observed_instances mismatch")
        if awaiting_action.get("current_observed_instances") != 1:
            raise RuntimeError("topology actuation realization current_observed_instances mismatch")

        actuation_adapter_body_v3 = {
            **actuation_adapter_body_v1,
            "execution_revision": 3,
            "expected_revision": 2,
        }
        status, _, payload = request_json_body(
            actuation_adapter_path,
            method="PUT",
            body_obj=actuation_adapter_body_v3,
        )
        if status != 200:
            raise RuntimeError(f"topology actuation adapter PUT v3 expected 200, got {status}")
        adapter_response_v3 = (payload or {}).get("data", {})
        adapter_v3 = adapter_response_v3.get("lease", {})
        if adapter_response_v3.get("present") is not True:
            raise RuntimeError("topology actuation adapter PUT v3 should mark lease present")
        if adapter_v3.get("revision") != 3:
            raise RuntimeError("topology actuation adapter PUT v3 revision mismatch")
        if adapter_v3.get("execution_revision") != 3:
            raise RuntimeError("topology actuation adapter PUT v3 execution revision mismatch")

        actuation_adapter_status_awaiting = load_json(f"{actuation_adapter_status_path}?timeout_ms=5000")
        if actuation_adapter_status_awaiting.get("data", {}).get("summary", {}).get("awaiting_realization_actions") != 1:
            raise RuntimeError("topology actuation adapter status awaiting_realization_actions mismatch after adapter PUT v3")

        status, _, payload = request_json_body(
            actuation_adapter_path,
            method="PUT",
            body_obj={
                **actuation_adapter_body_v1,
                "execution_revision": 3,
                "expected_revision": 0,
            },
        )
        if status != 409:
            raise RuntimeError(f"topology actuation adapter stale PUT expected 409, got {status}")
        if (payload or {}).get("error", {}).get("code") != "REVISION_MISMATCH":
            raise RuntimeError("topology actuation adapter stale PUT expected REVISION_MISMATCH")

        status, _, payload = request_json(actuation_adapter_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"topology actuation adapter DELETE expected 200, got {status}")
        actuation_adapter_after_delete = (payload or {}).get("data", {})
        if actuation_adapter_after_delete.get("present") is not False:
            raise RuntimeError("topology actuation adapter DELETE should clear lease")
        if actuation_adapter_after_delete.get("lease") is not None:
            raise RuntimeError("topology actuation adapter DELETE should return null lease")

        status, _, payload = request_json(actuation_execution_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"topology actuation execution DELETE expected 200, got {status}")
        actuation_execution_after_delete = (payload or {}).get("data", {})
        if actuation_execution_after_delete.get("present") is not False:
            raise RuntimeError("topology actuation execution DELETE should clear execution")
        if actuation_execution_after_delete.get("execution") is not None:
            raise RuntimeError("topology actuation execution DELETE should return null execution")

        status, _, payload = request_json(actuation_request_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"topology actuation request DELETE expected 200, got {status}")
        actuation_request_after_delete = (payload or {}).get("data", {})
        if actuation_request_after_delete.get("present") is not False:
            raise RuntimeError("topology actuation request DELETE should clear request")
        if actuation_request_after_delete.get("request") is not None:
            raise RuntimeError("topology actuation request DELETE should return null request")

        actuation_status_after_request_delete = load_json(actuation_status_path)
        actuation_status_after_request_delete_data = actuation_status_after_request_delete.get("data", {})
        if actuation_status_after_request_delete_data.get("summary", {}).get("request_present") is not False:
            raise RuntimeError("topology actuation status should report request_present=false after request DELETE")
        if actuation_status_after_request_delete_data.get("summary", {}).get("actions_total") != 0:
            raise RuntimeError("topology actuation status should report zero actions after request DELETE")

        status, _, payload = request_json(desired_topology_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"desired topology DELETE expected 200, got {status}")
        desired_after_delete = (payload or {}).get("data", {})
        if desired_after_delete.get("present") is not False:
            raise RuntimeError("desired topology DELETE should clear document")
        if desired_after_delete.get("topology") is not None:
            raise RuntimeError("desired topology DELETE should return null topology")

        actuation_after_delete = load_json(actuation_topology_path)
        actuation_after_delete_data = actuation_after_delete.get("data", {})
        if actuation_after_delete_data.get("desired") is not None:
            raise RuntimeError("topology actuation DELETE follow-up should report null desired")
        delete_observed_pools = actuation_after_delete_data.get("observed_pools", [])
        delete_actions = actuation_after_delete_data.get("actions", [])
        delete_summary = actuation_after_delete_data.get("summary", {})
        if delete_summary.get("actions_total") != len(delete_actions):
            raise RuntimeError("topology actuation DELETE follow-up actions_total should match actions length")
        if delete_summary.get("actions_total") != len(delete_observed_pools):
            raise RuntimeError("topology actuation DELETE follow-up should surface one observe-only action per observed pool")
        if delete_summary.get("actionable_actions") != 0:
            raise RuntimeError("topology actuation DELETE follow-up should not report actionable actions")
        if delete_summary.get("observe_only_actions") != len(delete_actions):
            raise RuntimeError("topology actuation DELETE follow-up should classify all actions as observe-only")
        for action in delete_actions:
            if action.get("action") != "observe_undeclared_pool":
                raise RuntimeError("topology actuation DELETE follow-up action should be observe_undeclared_pool")
            if action.get("actionable") is not False:
                raise RuntimeError("topology actuation DELETE follow-up observe-only action should not be actionable")

        runtime_assignment_target = first_server_for_other_world(
            STACK_TOPOLOGY,
            str(world_scope.get("world_id")),
        )
        if runtime_assignment_target is None:
            raise RuntimeError("active topology does not define a live runtime-assignment target")

        desired_body_runtime_v1 = {
            "topology_id": "runtime-assignment-topology",
            "pools": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "replicas": 2,
                    "capacity_class": "medium",
                    "placement_tags": ["region:dev", "tier:starter"],
                }
            ],
        }
        status, _, payload = request_json_body(
            desired_topology_path,
            method="PUT",
            body_obj=desired_body_runtime_v1,
        )
        if status != 200:
            raise RuntimeError(f"runtime assignment desired topology PUT expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("topology", {}) or {}).get("revision") != 1:
            raise RuntimeError("runtime assignment desired topology revision mismatch")

        actuation_request_body_runtime_v1 = {
            "request_id": "runtime-assignment-request",
            "basis_topology_revision": 1,
            "actions": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "action": "scale_out_pool",
                    "replica_delta": 1,
                }
            ],
        }
        status, _, payload = request_json_body(
            actuation_request_path,
            method="PUT",
            body_obj=actuation_request_body_runtime_v1,
        )
        if status != 200:
            raise RuntimeError(f"runtime assignment actuation request PUT expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("request", {}) or {}).get("revision") != 1:
            raise RuntimeError("runtime assignment actuation request revision mismatch")

        actuation_execution_body_runtime_v1 = {
            "executor_id": "runtime-assignment-executor",
            "request_revision": 1,
            "actions": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "action": "scale_out_pool",
                    "replica_delta": 1,
                    "observed_instances_before": 1,
                    "ready_instances_before": 1,
                    "state": "claimed",
                }
            ],
        }
        status, _, payload = request_json_body(
            actuation_execution_path,
            method="PUT",
            body_obj=actuation_execution_body_runtime_v1,
        )
        if status != 200:
            raise RuntimeError(f"runtime assignment actuation execution PUT expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("execution", {}) or {}).get("revision") != 1:
            raise RuntimeError("runtime assignment actuation execution revision mismatch")

        actuation_adapter_body_runtime_v1 = {
            "adapter_id": "runtime-assignment-adapter",
            "execution_revision": 1,
            "actions": [
                {
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "action": "scale_out_pool",
                    "replica_delta": 1,
                }
            ],
        }
        status, _, payload = request_json_body(
            actuation_adapter_path,
            method="PUT",
            body_obj=actuation_adapter_body_runtime_v1,
        )
        if status != 200:
            raise RuntimeError(f"runtime assignment adapter PUT v1 expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("lease", {}) or {}).get("revision") != 1:
            raise RuntimeError("runtime assignment adapter lease revision mismatch")

        runtime_assignment_body_v1 = {
            "adapter_id": "runtime-assignment-adapter",
            "lease_revision": 1,
            "assignments": [
                {
                    "instance_id": str(runtime_assignment_target.get("instance_id")),
                    "world_id": str(world_scope.get("world_id")),
                    "shard": str(world_server.get("shard")),
                    "action": "scale_out_pool",
                }
            ],
        }
        status, _, payload = request_json_body(
            actuation_runtime_assignment_path,
            method="PUT",
            body_obj=runtime_assignment_body_v1,
        )
        if status != 200:
            raise RuntimeError(f"runtime assignment PUT expected 200, got {status}")
        runtime_assignment_response_v1 = (payload or {}).get("data", {})
        runtime_assignment_v1 = runtime_assignment_response_v1.get("assignment", {})
        if runtime_assignment_response_v1.get("present") is not True:
            raise RuntimeError("runtime assignment PUT should mark document present")
        if runtime_assignment_v1.get("revision") != 1:
            raise RuntimeError("runtime assignment PUT revision mismatch")
        if runtime_assignment_v1.get("lease_revision") != 1:
            raise RuntimeError("runtime assignment PUT lease_revision mismatch")

        deadline = time.time() + 20.0
        live_assignment_satisfied = None
        while time.time() < deadline:
            live_assignment_satisfied = load_json(f"{actuation_status_path}?timeout_ms=5000")
            satisfied_summary = live_assignment_satisfied.get("data", {}).get("summary", {})
            if satisfied_summary.get("satisfied_actions") == 1:
                break
            time.sleep(0.5)
        if live_assignment_satisfied is None or live_assignment_satisfied.get("data", {}).get("summary", {}).get("satisfied_actions") != 1:
            raise RuntimeError("runtime assignment did not satisfy the actuation request before timeout")

        runtime_observed_after_assignment = load_json("/api/v1/topology/observed?timeout_ms=5000")
        runtime_worlds_after_assignment = runtime_observed_after_assignment.get("data", {}).get("worlds", [])
        current_world_after_assignment = next(
            (
                item
                for item in runtime_worlds_after_assignment
                if item.get("world_id") == world_scope.get("world_id")
            ),
            None,
        )
        if not isinstance(current_world_after_assignment, dict):
            raise RuntimeError("runtime assignment observed topology missing current world entry")
        if len(current_world_after_assignment.get("instances", [])) != 2:
            raise RuntimeError("runtime assignment should move one idle server into the current world")

        status, _, payload = request_json_body(
            actuation_execution_path,
            method="PUT",
            body_obj={
                **actuation_execution_body_runtime_v1,
                "expected_revision": 1,
                "actions": [
                    {
                        "world_id": str(world_scope.get("world_id")),
                        "shard": str(world_server.get("shard")),
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
            raise RuntimeError(f"runtime assignment actuation execution PUT v2 expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("execution", {}) or {}).get("revision") != 2:
            raise RuntimeError("runtime assignment actuation execution PUT v2 revision mismatch")

        status, _, payload = request_json_body(
            actuation_adapter_path,
            method="PUT",
            body_obj={
                **actuation_adapter_body_runtime_v1,
                "execution_revision": 2,
                "expected_revision": 1,
            },
        )
        if status != 200:
            raise RuntimeError(f"runtime assignment adapter PUT v2 expected 200, got {status}")
        if ((payload or {}).get("data", {}).get("lease", {}) or {}).get("revision") != 2:
            raise RuntimeError("runtime assignment adapter PUT v2 revision mismatch")

        runtime_adapter_status_realized = load_json(f"{actuation_adapter_status_path}?timeout_ms=5000")
        if runtime_adapter_status_realized.get("data", {}).get("summary", {}).get("realized_actions") != 1:
            raise RuntimeError("runtime assignment adapter status realized_actions mismatch")

        status, _, payload = request_json(actuation_runtime_assignment_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"runtime assignment DELETE expected 200, got {status}")
        runtime_assignment_after_delete = (payload or {}).get("data", {})
        if runtime_assignment_after_delete.get("present") is not False:
            raise RuntimeError("runtime assignment DELETE should clear the document")
        if runtime_assignment_after_delete.get("assignment") is not None:
            raise RuntimeError("runtime assignment DELETE should return null assignment")

        deadline = time.time() + 20.0
        reverted_actuation = None
        while time.time() < deadline:
            reverted_actuation = load_json(f"{actuation_topology_path}?timeout_ms=5000")
            reverted_summary = reverted_actuation.get("data", {}).get("summary", {})
            if reverted_summary.get("actionable_actions") == 1 and reverted_summary.get("observe_only_actions") == 1:
                break
            time.sleep(0.5)
        if reverted_actuation is None or reverted_actuation.get("data", {}).get("summary", {}).get("actionable_actions") != 1:
            raise RuntimeError("runtime assignment DELETE did not restore observed topology before timeout")

        status, _, payload = request_json(actuation_adapter_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"runtime assignment adapter DELETE expected 200, got {status}")
        status, _, payload = request_json(actuation_execution_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"runtime assignment actuation execution DELETE expected 200, got {status}")
        status, _, payload = request_json(actuation_request_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"runtime assignment actuation request DELETE expected 200, got {status}")
        status, _, payload = request_json(desired_topology_path, method="DELETE")
        if status != 200:
            raise RuntimeError(f"runtime assignment desired topology DELETE expected 200, got {status}")

        metrics_after_actuation = load_text("/metrics")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_requests_total"
            )
            <= topology_actuation_requests_before
        ):
            raise RuntimeError("admin topology actuation requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_request_requests_total"
            )
            <= topology_actuation_request_requests_before
        ):
            raise RuntimeError("admin topology actuation request requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_request_write_total"
            )
            <= topology_actuation_request_write_before
        ):
            raise RuntimeError("admin topology actuation request write metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_request_write_fail_total"
            )
            <= topology_actuation_request_write_fail_before
        ):
            raise RuntimeError("admin topology actuation request write_fail metric did not increase on stale revision")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_request_clear_total"
            )
            <= topology_actuation_request_clear_before
        ):
            raise RuntimeError("admin topology actuation request clear metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_request_clear_fail_total"
            )
            != topology_actuation_request_clear_fail_before
        ):
            raise RuntimeError("admin topology actuation request clear_fail metric changed unexpectedly")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_status_requests_total"
            )
            <= topology_actuation_status_requests_before
        ):
            raise RuntimeError("admin topology actuation status requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_execution_requests_total"
            )
            <= topology_actuation_execution_requests_before
        ):
            raise RuntimeError("admin topology actuation execution requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_execution_write_total"
            )
            <= topology_actuation_execution_write_before
        ):
            raise RuntimeError("admin topology actuation execution write metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_execution_write_fail_total"
            )
            <= topology_actuation_execution_write_fail_before
        ):
            raise RuntimeError("admin topology actuation execution write_fail metric did not increase on stale revision")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_execution_clear_total"
            )
            <= topology_actuation_execution_clear_before
        ):
            raise RuntimeError("admin topology actuation execution clear metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_execution_clear_fail_total"
            )
            != topology_actuation_execution_clear_fail_before
        ):
            raise RuntimeError("admin topology actuation execution clear_fail metric changed unexpectedly")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_execution_status_requests_total"
            )
            <= topology_actuation_execution_status_requests_before
        ):
            raise RuntimeError("admin topology actuation execution status requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_realization_requests_total"
            )
            <= topology_actuation_realization_requests_before
        ):
            raise RuntimeError("admin topology actuation realization requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_adapter_requests_total"
            )
            <= topology_actuation_adapter_requests_before
        ):
            raise RuntimeError("admin topology actuation adapter requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_adapter_write_total"
            )
            <= topology_actuation_adapter_write_before
        ):
            raise RuntimeError("admin topology actuation adapter write metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_adapter_write_fail_total"
            )
            <= topology_actuation_adapter_write_fail_before
        ):
            raise RuntimeError("admin topology actuation adapter write_fail metric did not increase on stale revision")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_adapter_clear_total"
            )
            <= topology_actuation_adapter_clear_before
        ):
            raise RuntimeError("admin topology actuation adapter clear metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_adapter_clear_fail_total"
            )
            != topology_actuation_adapter_clear_fail_before
        ):
            raise RuntimeError("admin topology actuation adapter clear_fail metric changed unexpectedly")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_adapter_status_requests_total"
            )
            <= topology_actuation_adapter_status_requests_before
        ):
            raise RuntimeError("admin topology actuation adapter status requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_runtime_assignment_requests_total"
            )
            <= topology_actuation_runtime_assignment_requests_before
        ):
            raise RuntimeError("admin topology actuation runtime assignment requests metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_runtime_assignment_write_total"
            )
            <= topology_actuation_runtime_assignment_write_before
        ):
            raise RuntimeError("admin topology actuation runtime assignment write metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_runtime_assignment_write_fail_total"
            )
            != topology_actuation_runtime_assignment_write_fail_before
        ):
            raise RuntimeError("admin topology actuation runtime assignment write_fail metric changed unexpectedly")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_runtime_assignment_clear_total"
            )
            <= topology_actuation_runtime_assignment_clear_before
        ):
            raise RuntimeError("admin topology actuation runtime assignment clear metric did not increase")
        if (
            parse_prometheus_counter(
                metrics_after_actuation, "admin_topology_actuation_runtime_assignment_clear_fail_total"
            )
            != topology_actuation_runtime_assignment_clear_fail_before
        ):
            raise RuntimeError("admin topology actuation runtime assignment clear_fail metric changed unexpectedly")

        metrics_after_topology = load_text("/metrics")
        if parse_prometheus_counter(
            metrics_after_topology, "admin_desired_topology_requests_total"
        ) <= desired_topology_requests_before:
            raise RuntimeError("desired topology requests metric did not increase")
        if parse_prometheus_counter(
            metrics_after_topology, "admin_desired_topology_write_total"
        ) < desired_topology_write_before + 2:
            raise RuntimeError("desired topology write metric did not reflect successful PUTs")
        if parse_prometheus_counter(
            metrics_after_topology, "admin_desired_topology_write_fail_total"
        ) <= desired_topology_write_fail_before:
            raise RuntimeError("desired topology write_fail metric did not increase on stale revision")
        if parse_prometheus_counter(
            metrics_after_topology, "admin_desired_topology_clear_total"
        ) <= desired_topology_clear_before:
            raise RuntimeError("desired topology clear metric did not increase")
        if parse_prometheus_counter(
            metrics_after_topology, "admin_desired_topology_clear_fail_total"
        ) != desired_topology_clear_fail_before:
            raise RuntimeError("desired topology clear_fail metric changed unexpectedly")
        if parse_prometheus_counter(
            metrics_after_topology, "admin_observed_topology_requests_total"
        ) <= observed_topology_requests_before:
            raise RuntimeError("observed topology requests metric did not increase")

        links = load_json("/api/v1/metrics/links")
        links_data = links.get("data", {})
        if "grafana" not in links_data or "prometheus" not in links_data:
            raise RuntimeError("metrics links payload missing grafana/prometheus")

        status, _, payload = request_json("/api/v1/instances?limit=501")
        if status != 400:
            raise RuntimeError(
                f"/api/v1/instances?limit=501 expected 400, got {status}"
            )
        error = (payload or {}).get("error", {})
        if error.get("code") != "BAD_REQUEST":
            raise RuntimeError("limit validation expected BAD_REQUEST")
        if "details" not in error:
            raise RuntimeError("limit validation error.details missing")

        status, _, payload = request_json("/api/v1/instances?cursor=abc")
        if status != 400:
            raise RuntimeError(
                f"/api/v1/instances?cursor=abc expected 400, got {status}"
            )
        error = (payload or {}).get("error", {})
        if error.get("code") != "BAD_REQUEST":
            raise RuntimeError("cursor validation expected BAD_REQUEST")
        if "details" not in error:
            raise RuntimeError("cursor validation error.details missing")

        status, _, payload = request_json("/api/v1/instances?timeout_ms=6000")
        if status != 400:
            raise RuntimeError(
                f"/api/v1/instances?timeout_ms=6000 expected 400, got {status}"
            )
        error = (payload or {}).get("error", {})
        if error.get("code") != "BAD_REQUEST":
            raise RuntimeError("timeout_ms validation expected BAD_REQUEST")
        if "details" not in error:
            raise RuntimeError("timeout validation error.details missing")

        try:
            status, content_type, body = http_request("/api/v1/overview", method="POST")
        except urllib.error.HTTPError as exc:
            status = exc.code
            content_type = exc.headers.get("Content-Type", "")
            body = exc.read()

        if status != 405:
            raise RuntimeError(f"POST /api/v1/overview expected 405, got {status}")
        if "application/json" not in content_type:
            raise RuntimeError("POST /api/v1/overview expected json error response")
        payload = json.loads(body.decode("utf-8", errors="replace"))
        if payload.get("error", {}).get("code") != "METHOD_NOT_ALLOWED":
            raise RuntimeError("POST /api/v1/overview expected METHOD_NOT_ALLOWED")
        if "details" not in payload.get("error", {}):
            raise RuntimeError("POST /api/v1/overview error.details missing")

        print("PASS: admin API and UI smoke test")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
