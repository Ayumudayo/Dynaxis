import json
import os
import time
import urllib.error
import urllib.request


BASE_URL = "http://127.0.0.1:39200"


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
        for key in ("disconnect", "announce", "settings", "moderation", "ext_deploy"):
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

        detail = load_json(f"/api/v1/instances/{instance_id}")
        detail_data = detail.get("data", {})
        if not detail_data.get("metrics_url"):
            raise RuntimeError("instance detail missing metrics_url")
        if not detail_data.get("ready_reason"):
            raise RuntimeError("instance detail missing ready_reason")

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
