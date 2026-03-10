import json
import socket
import subprocess
import time
import urllib.error
import urllib.request


IMAGE = "dynaxis-admin:local"


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
    raise RuntimeError(f"admin read-only test container not ready: {last_error}")


def request_with_payload(url: str, headers=None, method: str = "GET"):
    req = urllib.request.Request(url, headers=headers or {}, method=method)
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


def request_text(url: str, headers=None, method: str = "GET"):
    req = urllib.request.Request(url, headers=headers or {}, method=method)
    with urllib.request.urlopen(req, timeout=5) as response:
        return response.status, response.read().decode("utf-8", errors="replace")


def expect_error(
    base_url: str,
    path: str,
    expected_status: int,
    expected_code: str,
    headers=None,
    method: str = "GET",
) -> None:
    status, content_type, payload, _ = request_with_payload(
        f"{base_url}{path}",
        headers=headers,
        method=method,
    )
    if status != expected_status:
        raise RuntimeError(f"{method} {path} expected {expected_status}, got {status}")
    if "application/json" not in content_type:
        raise RuntimeError(f"{method} {path} expected json error response")
    error = (payload or {}).get("error", {})
    if error.get("code") != expected_code:
        raise RuntimeError(f"{method} {path} expected error code {expected_code}, got {error.get('code')}")


def expect_not_forbidden(base_url: str, path: str, headers=None, method: str = "GET") -> int:
    status, _, payload, body = request_with_payload(
        f"{base_url}{path}",
        headers=headers,
        method=method,
    )
    if status == 403:
        raise RuntimeError(f"{method} {path} should not be forbidden")
    if (payload or {}).get("error", {}).get("code") == "FORBIDDEN":
        raise RuntimeError(f"{method} {path} returned FORBIDDEN unexpectedly: {body}")
    if (payload or {}).get("error", {}).get("code") == "READ_ONLY":
        raise RuntimeError(f"{method} {path} returned READ_ONLY unexpectedly: {body}")
    return status


def run_case(read_only: bool) -> None:
    container_name = f"admin-read-only-test-{'on' if read_only else 'off'}-{int(time.time())}"
    host_port = pick_free_port()
    base_url = f"http://127.0.0.1:{host_port}"

    run_cmd = [
        "docker", "run", "-d",
        "--name", container_name,
        "-p", f"{host_port}:39200",
        "-e", "METRICS_PORT=39200",
        "-e", "ADMIN_AUTH_MODE=header",
        "-e", "ADMIN_AUTH_USER_HEADER=X-Admin-User",
        "-e", "ADMIN_AUTH_ROLE_HEADER=X-Admin-Role",
        "-e", f"ADMIN_READ_ONLY={'1' if read_only else '0'}",
        IMAGE,
        "admin",
    ]

    viewer_headers = {"X-Admin-User": "viewer-user", "X-Admin-Role": "viewer"}
    operator_headers = {"X-Admin-User": "operator-user", "X-Admin-Role": "operator"}
    admin_headers = {"X-Admin-User": "admin-user", "X-Admin-Role": "admin"}

    disconnect_path = "/api/v1/users/disconnect?client_id=read-only-user&reason=read-only-check"
    announce_path = "/api/v1/announcements?text=read-only-announcement&priority=info"
    settings_path = "/api/v1/settings?key=recent_history_limit&value=35"
    moderation_path = "/api/v1/users/mute?client_id=read-only-user&duration_sec=30&reason=read-only-check"

    started = False
    try:
        subprocess.check_call(run_cmd)
        started = True
        wait_ready(base_url)

        status, _, payload, _ = request_with_payload(
            f"{base_url}/api/v1/auth/context",
            headers=admin_headers,
        )
        if status != 200:
            raise RuntimeError(f"auth context expected 200, got {status}")

        data = (payload or {}).get("data", {})
        capabilities = data.get("capabilities", {})
        read_only_value = data.get("read_only")

        if read_only_value is not read_only:
            raise RuntimeError(f"auth context read_only expected {read_only}, got {read_only_value}")

        if read_only:
            for key in ("disconnect", "announce", "settings", "moderation"):
                if capabilities.get(key) is not False:
                    raise RuntimeError(f"read-only capability {key} should be false")

            expect_error(base_url, disconnect_path, 403, "READ_ONLY", headers=admin_headers, method="POST")
            expect_error(base_url, announce_path, 403, "READ_ONLY", headers=admin_headers, method="POST")
            expect_error(base_url, settings_path, 403, "READ_ONLY", headers=admin_headers, method="PATCH")
            expect_error(base_url, moderation_path, 403, "READ_ONLY", headers=admin_headers, method="POST")
            expect_error(base_url, announce_path, 403, "READ_ONLY", headers=operator_headers, method="POST")
            expect_error(base_url, announce_path, 403, "READ_ONLY", headers=viewer_headers, method="POST")

            metrics_status, metrics_body = request_text(
                f"{base_url}/metrics",
                headers=admin_headers,
            )
            if metrics_status != 200:
                raise RuntimeError(f"metrics expected 200, got {metrics_status}")
            if "admin_read_only_mode 1" not in metrics_body:
                raise RuntimeError("metrics missing admin_read_only_mode 1")
        else:
            expected_caps = {
                "disconnect": True,
                "announce": True,
                "settings": True,
                "moderation": True,
            }
            for key, expected in expected_caps.items():
                if capabilities.get(key) is not expected:
                    raise RuntimeError(f"capability {key} expected {expected}, got {capabilities.get(key)}")

            expect_not_forbidden(base_url, disconnect_path, headers=admin_headers, method="POST")
            expect_not_forbidden(base_url, announce_path, headers=admin_headers, method="POST")
            expect_not_forbidden(base_url, settings_path, headers=admin_headers, method="PATCH")
            expect_not_forbidden(base_url, moderation_path, headers=admin_headers, method="POST")
            expect_not_forbidden(base_url, announce_path, headers=operator_headers, method="POST")
            expect_error(base_url, announce_path, 403, "FORBIDDEN", headers=viewer_headers, method="POST")

            metrics_status, metrics_body = request_text(
                f"{base_url}/metrics",
                headers=admin_headers,
            )
            if metrics_status != 200:
                raise RuntimeError(f"metrics expected 200, got {metrics_status}")
            if "admin_read_only_mode 0" not in metrics_body:
                raise RuntimeError("metrics missing admin_read_only_mode 0")
    finally:
        if started:
            subprocess.run(["docker", "rm", "-f", container_name], check=False)


def main() -> int:
    try:
        run_case(read_only=True)
        run_case(read_only=False)
        print("PASS: admin read-only mode smoke test")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
