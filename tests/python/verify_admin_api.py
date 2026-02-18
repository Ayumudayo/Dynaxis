import json
import time
import urllib.error
import urllib.request


BASE_URL = "http://127.0.0.1:39200"


def http_request(path: str, method: str = "GET"):
    req = urllib.request.Request(f"{BASE_URL}{path}", method=method)
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
        if "Knights Admin Console" not in html:
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
        if not isinstance(audit_trend["max_points"], int) or audit_trend["max_points"] <= 0:
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
                    raise RuntimeError(f"overview.audit_trend.points sample missing '{key}'")

        if "meta" not in overview or "request_id" not in overview["meta"]:
            raise RuntimeError("overview meta.request_id missing")

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
        for key in ("disconnect", "announce", "settings"):
            if key not in capabilities:
                raise RuntimeError(f"auth context capabilities missing '{key}'")
            if not isinstance(capabilities[key], bool):
                raise RuntimeError(f"auth context capabilities.{key} expected bool")

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
            raise RuntimeError(f"/api/v1/instances?limit=501 expected 400, got {status}")
        error = (payload or {}).get("error", {})
        if error.get("code") != "BAD_REQUEST":
            raise RuntimeError("limit validation expected BAD_REQUEST")
        if "details" not in error:
            raise RuntimeError("limit validation error.details missing")

        status, _, payload = request_json("/api/v1/instances?cursor=abc")
        if status != 400:
            raise RuntimeError(f"/api/v1/instances?cursor=abc expected 400, got {status}")
        error = (payload or {}).get("error", {})
        if error.get("code") != "BAD_REQUEST":
            raise RuntimeError("cursor validation expected BAD_REQUEST")
        if "details" not in error:
            raise RuntimeError("cursor validation error.details missing")

        status, _, payload = request_json("/api/v1/instances?timeout_ms=6000")
        if status != 400:
            raise RuntimeError(f"/api/v1/instances?timeout_ms=6000 expected 400, got {status}")
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
