import json
import socket
import subprocess
import time
import urllib.error
import urllib.request


IMAGE = "dynaxis-admin:local"
TOKEN = "ci-admin-token"


def pick_free_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return int(port)


def wait_ready(base_url: str, timeout_sec: float = 30.0) -> None:
    deadline = time.time() + timeout_sec
    last_error = "unknown"
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"{base_url}/healthz", timeout=5) as response:
                if response.status == 200:
                    return
                last_error = f"status={response.status}"
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.5)
    raise RuntimeError(f"admin auth test container not ready: {last_error}")


def request_with_payload(url: str, headers=None, method: str = "GET", data: bytes | None = None):
    req = urllib.request.Request(url, headers=headers or {}, method=method, data=data)
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            status = response.status
            content_type = response.getheader("Content-Type", "")
            body = response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status = exc.code
        content_type = exc.headers.get("Content-Type", "")
        body = exc.read().decode("utf-8", errors="replace")

    payload = None
    if body and "application/json" in content_type:
        payload = json.loads(body)
    return status, content_type, payload, body


def request_text(url: str, headers=None, method: str = "GET") -> tuple[int, str]:
    req = urllib.request.Request(url, headers=headers or {}, method=method)
    with urllib.request.urlopen(req, timeout=5) as response:
        return response.status, response.read().decode("utf-8", errors="replace")


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


def expect_error(
    base_url: str,
    path: str,
    expected_status: int,
    expected_code: str,
    headers=None,
    method: str = "GET",
    data: bytes | None = None,
) -> None:
    status, content_type, payload, _ = request_with_payload(
        f"{base_url}{path}",
        headers=headers,
        method=method,
        data=data,
    )
    if status != expected_status:
        raise RuntimeError(f"{path} expected {expected_status}, got {status}")
    if "application/json" not in content_type:
        raise RuntimeError(f"{path} expected json error response")
    error = (payload or {}).get("error", {})
    if error.get("code") != expected_code:
        raise RuntimeError(f"{path} expected error code {expected_code}")


def expect_not_forbidden(base_url: str, path: str, headers=None, method: str = "GET", data: bytes | None = None) -> int:
    status, _, payload, body = request_with_payload(
        f"{base_url}{path}",
        headers=headers,
        method=method,
        data=data,
    )
    if status == 403:
        raise RuntimeError(f"{method} {path} should not be forbidden")
    if (payload or {}).get("error", {}).get("code") == "FORBIDDEN":
        raise RuntimeError(f"{method} {path} returned FORBIDDEN unexpectedly: {body}")
    return status


def main() -> int:
    container_name = f"admin-auth-test-{int(time.time())}"
    host_port = pick_free_port()
    base_url = f"http://127.0.0.1:{host_port}"

    run_cmd = [
        "docker", "run", "-d",
        "--name", container_name,
        "-p", f"{host_port}:39200",
        "-e", "METRICS_PORT=39200",
        "-e", "ADMIN_AUTH_MODE=header_or_bearer",
        "-e", "ADMIN_AUTH_USER_HEADER=X-Admin-User",
        "-e", "ADMIN_AUTH_ROLE_HEADER=X-Admin-Role",
        "-e", f"ADMIN_BEARER_TOKEN={TOKEN}",
        "-e", "ADMIN_BEARER_ACTOR=ci-bearer",
        "-e", "ADMIN_BEARER_ROLE=viewer",
        IMAGE,
        "admin",
    ]

    started = False
    try:
        subprocess.check_call(run_cmd)
        started = True

        wait_ready(base_url)

        expect_error(base_url, "/admin", 401, "UNAUTHORIZED")
        expect_error(base_url, "/api/v1/overview", 401, "UNAUTHORIZED")
        expect_error(
            base_url,
            "/api/v1/overview",
            403,
            "FORBIDDEN",
            headers={"X-Admin-User": "ops", "X-Admin-Role": "guest"},
        )

        status, content_type, payload, _ = request_with_payload(
            f"{base_url}/api/v1/overview",
            headers={"X-Admin-User": "ops", "X-Admin-Role": "viewer"},
        )
        if status != 200:
            raise RuntimeError(f"header auth request expected 200, got {status}")
        if "application/json" not in content_type:
            raise RuntimeError("header auth request expected json content-type")
        if (payload or {}).get("data", {}).get("service") != "admin_app":
            raise RuntimeError("header auth request returned unexpected payload")

        status, _, payload, _ = request_with_payload(
            f"{base_url}/api/v1/auth/context",
            headers={"X-Admin-User": "ops", "X-Admin-Role": "viewer"},
        )
        if status != 200:
            raise RuntimeError(f"header auth context expected 200, got {status}")
        auth_data = (payload or {}).get("data", {})
        if auth_data.get("actor") != "ops" or auth_data.get("role") != "viewer":
            raise RuntimeError("header auth context mismatch")
        if auth_data.get("mode") != "header_or_bearer":
            raise RuntimeError("header auth context mode mismatch")

        status, _, payload, _ = request_with_payload(
            f"{base_url}/api/v1/overview",
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        if status != 200:
            raise RuntimeError(f"bearer auth request expected 200, got {status}")
        if (payload or {}).get("data", {}).get("service") != "admin_app":
            raise RuntimeError("bearer auth request returned unexpected payload")

        status, _, payload, _ = request_with_payload(
            f"{base_url}/api/v1/auth/context",
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        if status != 200:
            raise RuntimeError(f"bearer auth context expected 200, got {status}")
        auth_data = (payload or {}).get("data", {})
        if auth_data.get("actor") != "ci-bearer" or auth_data.get("role") != "viewer":
            raise RuntimeError("bearer auth context mismatch")
        if auth_data.get("mode") != "header_or_bearer":
            raise RuntimeError("bearer auth context mode mismatch")

        viewer_headers = {"X-Admin-User": "viewer-user", "X-Admin-Role": "viewer"}
        operator_headers = {"X-Admin-User": "operator-user", "X-Admin-Role": "operator"}
        admin_headers = {"X-Admin-User": "admin-user", "X-Admin-Role": "admin"}

        expected_caps = [
            (
                viewer_headers,
                {
                    "disconnect": False,
                    "announce": False,
                    "settings": False,
                    "moderation": False,
                    "world_policy": False,
                },
            ),
            (
                operator_headers,
                {
                    "disconnect": False,
                    "announce": True,
                    "settings": False,
                    "moderation": False,
                    "world_policy": False,
                },
            ),
            (
                admin_headers,
                {
                    "disconnect": True,
                    "announce": True,
                    "settings": True,
                    "moderation": True,
                    "world_policy": True,
                },
            ),
        ]

        for headers, expected in expected_caps:
            status, _, payload, _ = request_with_payload(
                f"{base_url}/api/v1/auth/context",
                headers=headers,
            )
            if status != 200:
                raise RuntimeError(f"auth context role matrix expected 200, got {status}")
            capabilities = (payload or {}).get("data", {}).get("capabilities", {})
            for key, expected_value in expected.items():
                if capabilities.get(key) is not expected_value:
                    raise RuntimeError(
                        f"capability mismatch for role={headers['X-Admin-Role']} key={key}: expected {expected_value}, got {capabilities.get(key)}"
                    )

        disconnect_path = "/api/v1/users/disconnect?client_id=role-matrix-user&reason=auth-check"
        announce_path = "/api/v1/announcements?text=role-matrix-announcement&priority=info"
        settings_path = "/api/v1/settings?key=recent_history_limit&value=35"
        moderation_path = "/api/v1/users/mute?client_id=role-matrix-user&duration_sec=30&reason=auth-check"
        world_policy_path = "/api/v1/worlds/test-world/policy"
        world_policy_body = json.dumps(
            {"draining": True, "replacement_owner_instance_id": None}
        ).encode("utf-8")
        json_headers = {"Content-Type": "application/json", "Content-Length": str(len(world_policy_body))}

        expect_error(
            base_url,
            disconnect_path,
            403,
            "FORBIDDEN",
            headers=viewer_headers,
            method="POST",
        )
        expect_error(
            base_url,
            announce_path,
            403,
            "FORBIDDEN",
            headers=viewer_headers,
            method="POST",
        )
        expect_error(
            base_url,
            settings_path,
            403,
            "FORBIDDEN",
            headers=viewer_headers,
            method="PATCH",
        )
        expect_error(
            base_url,
            moderation_path,
            403,
            "FORBIDDEN",
            headers=viewer_headers,
            method="POST",
        )
        expect_error(
            base_url,
            world_policy_path,
            403,
            "FORBIDDEN",
            headers={**viewer_headers, **json_headers},
            method="PUT",
            data=world_policy_body,
        )

        expect_error(
            base_url,
            disconnect_path,
            403,
            "FORBIDDEN",
            headers=operator_headers,
            method="POST",
        )
        expect_error(
            base_url,
            settings_path,
            403,
            "FORBIDDEN",
            headers=operator_headers,
            method="PATCH",
        )
        expect_error(
            base_url,
            moderation_path,
            403,
            "FORBIDDEN",
            headers=operator_headers,
            method="POST",
        )
        expect_error(
            base_url,
            world_policy_path,
            403,
            "FORBIDDEN",
            headers={**operator_headers, **json_headers},
            method="PUT",
            data=world_policy_body,
        )
        expect_not_forbidden(
            base_url,
            announce_path,
            headers=operator_headers,
            method="POST",
        )

        expect_not_forbidden(
            base_url,
            disconnect_path,
            headers=admin_headers,
            method="POST",
        )
        expect_not_forbidden(
            base_url,
            announce_path,
            headers=admin_headers,
            method="POST",
        )
        expect_not_forbidden(
            base_url,
            settings_path,
            headers=admin_headers,
            method="PATCH",
        )
        expect_not_forbidden(
            base_url,
            moderation_path,
            headers=admin_headers,
            method="POST",
        )
        expect_not_forbidden(
            base_url,
            world_policy_path,
            headers={**admin_headers, **json_headers},
            method="PUT",
            data=world_policy_body,
        )

        metrics_status, metrics_body = request_text(
            f"{base_url}/metrics",
            headers=admin_headers,
        )
        if metrics_status != 200:
            raise RuntimeError(f"metrics expected 200, got {metrics_status}")
        for metric_name in (
            "admin_world_policy_write_total",
            "admin_world_policy_write_fail_total",
            "admin_world_policy_clear_total",
            "admin_world_policy_clear_fail_total",
        ):
            if parse_prometheus_counter(metrics_body, metric_name) != 0:
                raise RuntimeError(
                    f"{metric_name} should remain 0 in auth-only container"
                )

        expect_error(
            base_url,
            "/api/v1/overview",
            401,
            "UNAUTHORIZED",
            headers={"Authorization": "Bearer wrong-token"},
        )

        expect_error(
            base_url,
            "/api/v1/overview",
            403,
            "FORBIDDEN",
            headers={
                "X-Admin-User": "ops",
                "X-Admin-Role": "guest",
                "Authorization": f"Bearer {TOKEN}",
            },
        )

        status, content_type, _, body = request_with_payload(
            f"{base_url}/admin",
            headers={"X-Admin-User": "ops", "X-Admin-Role": "viewer"},
        )
        if status != 200:
            raise RuntimeError(f"authorized /admin expected 200, got {status}")
        if "text/html" not in content_type:
            raise RuntimeError("authorized /admin expected html content-type")
        if "Dynaxis Admin Console" not in body:
            raise RuntimeError("authorized /admin missing expected UI marker")

        print("PASS: admin auth mode smoke test")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        if started:
            subprocess.run(["docker", "rm", "-f", container_name], check=False)


if __name__ == "__main__":
    raise SystemExit(main())
