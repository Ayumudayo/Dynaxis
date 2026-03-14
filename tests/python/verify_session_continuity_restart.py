import argparse
import hashlib
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

from session_continuity_common import ChatClient
from session_continuity_common import MSG_CHAT_SEND
from session_continuity_common import lp_utf8
from session_continuity_common import read_metric_sum
from session_continuity_common import MSG_ERR

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPOSE_PROJECT_DIR = REPO_ROOT / "docker" / "stack"
COMPOSE_FILE = COMPOSE_PROJECT_DIR / "docker-compose.yml"
COMPOSE_ENV_FILE = COMPOSE_PROJECT_DIR / ".env.rudp-attach.example"
COMPOSE_PROJECT_NAME = "dynaxis-stack"
SESSION_DIRECTORY_PREFIX = "gateway/session/"
RESUME_LOCATOR_PREFIX = SESSION_DIRECTORY_PREFIX + "locator/"
CONTINUITY_WORLD_PREFIX = "dynaxis:continuity:world:"
ROOM_MISMATCH_ERRC = 0x0106

GATEWAY_READY_PORTS = {
    "gateway-1": 36001,
    "gateway-2": 36002,
}

SERVER_READY_PORTS = {
    "server-1": 39091,
    "server-2": 39092,
}


def compose_base_args() -> list[str]:
    args = [
        "docker",
        "compose",
        "--project-name",
        COMPOSE_PROJECT_NAME,
        "--project-directory",
        str(COMPOSE_PROJECT_DIR),
        "-f",
        str(COMPOSE_FILE),
    ]
    if COMPOSE_ENV_FILE.exists():
        args.extend(["--env-file", str(COMPOSE_ENV_FILE)])
    return args


def run_compose(*args: str) -> str:
    command = compose_base_args() + list(args)
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout.strip()


def redis_get(key: str) -> str:
    return run_compose("exec", "-T", "redis", "redis-cli", "--raw", "GET", key).strip()


def redis_del(key: str) -> None:
    run_compose("exec", "-T", "redis", "redis-cli", "DEL", key)


def make_resume_routing_key(resume_token: str) -> str:
    digest = hashlib.sha256(resume_token.encode("utf-8")).hexdigest()
    return f"resume-hash:{digest}"


def read_ready(port: int) -> tuple[int, str]:
    url = f"http://127.0.0.1:{port}/readyz"
    try:
        with urllib.request.urlopen(url, timeout=3.0) as response:
            return response.status, response.read().decode("utf-8").strip()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", errors="replace").strip()
    except Exception:
        return 0, ""


def wait_ready(port: int, timeout_sec: float = 60.0) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        status, body = read_ready(port)
        if status == 200 and body == "ready":
            return
        time.sleep(1.0)
    status, body = read_ready(port)
    raise TimeoutError(f"readyz timeout on {port}: status={status} body={body!r}")


def assert_initial_login(login: dict, user: str) -> tuple[str, str]:
    if login["effective_user"] != user:
        raise AssertionError(f"effective user mismatch: {login}")
    if not login["logical_session_id"] or not login["resume_token"] or login["resume_expires_unix_ms"] == 0:
        raise AssertionError(f"continuity lease fields missing: {login}")
    if not login["world_id"]:
        raise AssertionError(f"world admission metadata missing: {login}")
    if login["resumed"]:
        raise AssertionError(f"initial login unexpectedly marked resumed: {login}")
    return login["logical_session_id"], login["resume_token"]


def assert_resumed_login(login: dict, user: str, logical_session_id: str) -> None:
    if login["effective_user"] != user:
        raise AssertionError(f"resumed effective user mismatch: {login}")
    if login["logical_session_id"] != logical_session_id:
        raise AssertionError(f"logical session id changed across resume: {login}")
    if not login["resumed"]:
        raise AssertionError(f"resumed login not marked resumed: {login}")


def connect_and_login(user: str, token: str, host: str, port: int) -> tuple[ChatClient, dict]:
    client = ChatClient(host=host, port=port)
    client.connect()
    login = client.login(user, token)
    return client, login


def current_backend_for_user(user: str) -> str:
    return redis_get(f"{SESSION_DIRECTORY_PREFIX}{user}")


def current_backend_for_resume_token(resume_token: str) -> str:
    routing_key = make_resume_routing_key(resume_token)
    return redis_get(f"{SESSION_DIRECTORY_PREFIX}{routing_key}")


def current_locator_for_resume_token(resume_token: str) -> str:
    routing_key = make_resume_routing_key(resume_token)
    return redis_get(f"{RESUME_LOCATOR_PREFIX}{routing_key}")


def parse_locator_payload(payload: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in payload.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


def wait_for_redis_value(key: str, timeout_sec: float = 10.0) -> str:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        value = redis_get(key)
        if value:
            return value
        time.sleep(0.5)
    return redis_get(key)


def acquire_session_for_backend(target_backend: str, host: str, port: int, prefix: str) -> tuple[ChatClient, dict, str]:
    for attempt in range(1, 9):
        user = f"{prefix}_{int(time.time())}_{attempt}"
        client, login = connect_and_login(user, "", host, port)
        backend = current_backend_for_user(user)
        if backend == target_backend:
            return client, login, user
        client.close()
    raise RuntimeError(f"failed to land on backend {target_backend}")


def resume_until_success(host: str,
                         port: int,
                         resume_token: str,
                         user: str,
                         logical_session_id: str,
                         timeout_sec: float = 30.0) -> tuple[ChatClient, dict]:
    deadline = time.monotonic() + timeout_sec
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        client = ChatClient(host=host, port=port)
        try:
            client.connect()
            login = client.login("ignored_resume_user", "resume:" + resume_token)
            assert_resumed_login(login, user, logical_session_id)
            return client, login
        except Exception as exc:
            last_error = exc
            client.close()
            time.sleep(1.0)

    raise RuntimeError(f"resume retry window expired: {last_error}")


def restart_service(service: str) -> None:
    run_compose("restart", service)


def run_gateway_restart() -> None:
    user = f"verify_resume_gateway_{int(time.time())}"
    room = f"resume_gateway_room_{int(time.time())}"
    message = f"resume_gateway_msg_{int(time.time() * 1000)}"

    hit_before = read_metric_sum("gateway_resume_routing_hit_total")
    first, login = connect_and_login(user, "", "127.0.0.1", 36100)
    second = ChatClient(host="127.0.0.1", port=36101)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        first.join_room(room, user)

        print("Restarting gateway-1 and preserving resume alias...")
        alias_backend_before = wait_for_redis_value(f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}")
        if not alias_backend_before:
            raise AssertionError("resume routing alias was not persisted before gateway restart")

        restart_service("gateway-1")
        wait_ready(GATEWAY_READY_PORTS["gateway-1"])
        first.close()

        second, resumed = resume_until_success("127.0.0.1", 36101, resume_token, user, logical_session_id)

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        alias_backend_after = wait_for_redis_value(f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}")
        if alias_backend_after != alias_backend_before:
            raise AssertionError(
                f"resume routing alias changed across gateway restart: before={alias_backend_before} after={alias_backend_after}"
            )

        hit_after = read_metric_sum("gateway_resume_routing_hit_total")
        if hit_after <= hit_before:
            raise AssertionError(f"resume routing hit counter did not increase: before={hit_before} after={hit_after}")

        print(
            "PASS gateway-restart: "
            f"logical_session_id={logical_session_id} alias_backend={alias_backend_after} "
            f"resume_hit_delta={hit_after - hit_before:.0f}"
        )
    finally:
        first.close()
        second.close()


def run_server_restart() -> None:
    room = f"resume_server_room_{int(time.time())}"
    message = f"resume_server_msg_{int(time.time() * 1000)}"
    gateway_host = "127.0.0.1"
    gateway_port = 36101

    first, login, user = acquire_session_for_backend("server-1", gateway_host, gateway_port, "verify_resume_server")
    second = ChatClient(host=gateway_host, port=gateway_port)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        first.join_room(room, user)

        print("Restarting server-1 and waiting for continuity resume...")
        backend_before = wait_for_redis_value(f"{SESSION_DIRECTORY_PREFIX}{user}")
        alias_backend_before = wait_for_redis_value(f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}")
        if backend_before != "server-1":
            raise AssertionError(f"session was not attached to server-1 before restart: {backend_before}")
        if alias_backend_before != "server-1":
            raise AssertionError(f"resume alias was not attached to server-1 before restart: {alias_backend_before}")

        restart_service("server-1")
        wait_ready(SERVER_READY_PORTS["server-1"])
        first.close()

        second, resumed = resume_until_success(gateway_host, gateway_port, resume_token, user, logical_session_id)

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        backend_after = current_backend_for_user(user)
        if not backend_after:
            raise AssertionError("user backend routing was not restored after server restart")

        print(
            "PASS server-restart: "
            f"logical_session_id={logical_session_id} backend_before={backend_before} backend_after={backend_after}"
        )
    finally:
        first.close()
        second.close()


def run_locator_fallback() -> None:
    room = f"resume_locator_room_{int(time.time())}"
    message = f"resume_locator_msg_{int(time.time() * 1000)}"
    gateway_host = "127.0.0.1"
    gateway_port = 36101

    first, login, user = acquire_session_for_backend("server-1", gateway_host, gateway_port, "verify_resume_locator")
    second = ChatClient(host="127.0.0.1", port=36100)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != "starter-a":
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        routing_key = make_resume_routing_key(resume_token)
        alias_key = f"{SESSION_DIRECTORY_PREFIX}{routing_key}"
        locator_key = f"{RESUME_LOCATOR_PREFIX}{routing_key}"

        print("Dropping exact resume alias binding and forcing locator-based reconnect...")
        alias_backend_before = wait_for_redis_value(alias_key)
        locator_before = wait_for_redis_value(locator_key)
        locator_fields = parse_locator_payload(locator_before)
        if alias_backend_before != "server-1":
            raise AssertionError(f"resume alias was not attached to server-1 before locator fallback: {alias_backend_before}")
        if locator_fields.get("shard") != "stack-shard-a":
            raise AssertionError(f"resume locator hint did not capture shard boundary: {locator_before!r}")
        if locator_fields.get("world_id") != "starter-a":
            raise AssertionError(f"resume locator hint did not capture world admission metadata: {locator_before!r}")

        routing_hit_before = read_metric_sum("gateway_resume_routing_hit_total")
        selector_hit_before = read_metric_sum("gateway_resume_locator_selector_hit_total")
        selector_fallback_before = read_metric_sum("gateway_resume_locator_selector_fallback_total")

        redis_del(alias_key)
        if redis_get(alias_key):
            raise AssertionError("exact resume alias binding still exists after delete")
        first.close()

        second, resumed = resume_until_success("127.0.0.1", 36100, resume_token, user, logical_session_id)

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        alias_backend_after = wait_for_redis_value(alias_key)
        if alias_backend_after != "server-1":
            raise AssertionError(
                f"locator fallback did not restore the same shard/backend boundary: before={alias_backend_before} after={alias_backend_after}"
            )

        routing_hit_after = read_metric_sum("gateway_resume_routing_hit_total")
        selector_hit_after = read_metric_sum("gateway_resume_locator_selector_hit_total")
        selector_fallback_after = read_metric_sum("gateway_resume_locator_selector_fallback_total")

        if routing_hit_after != routing_hit_before:
            raise AssertionError(
                f"exact sticky hit counter changed during locator fallback: before={routing_hit_before} after={routing_hit_after}"
            )
        if selector_hit_after <= selector_hit_before:
            raise AssertionError(
                f"locator selector hit counter did not increase: before={selector_hit_before} after={selector_hit_after}"
            )
        if selector_fallback_after != selector_fallback_before:
            raise AssertionError(
                f"locator selector unexpectedly fell back to global routing: before={selector_fallback_before} after={selector_fallback_after}"
            )

        print(
            "PASS locator-fallback: "
            f"logical_session_id={logical_session_id} alias_backend_after={alias_backend_after} "
            f"selector_hit_delta={selector_hit_after - selector_hit_before:.0f}"
        )
    finally:
        first.close()
        second.close()


def run_world_residency_fallback() -> None:
    room = f"resume_world_room_{int(time.time())}"
    gateway_host = "127.0.0.1"
    gateway_port = 36101

    first, login, user = acquire_session_for_backend("server-1", gateway_host, gateway_port, "verify_resume_world")
    second = ChatClient(host="127.0.0.1", port=36100)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != "starter-a":
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        world_key = f"{CONTINUITY_WORLD_PREFIX}{logical_session_id}"
        print("Dropping persisted world residency and forcing safe fallback...")
        if wait_for_redis_value(world_key) != "starter-a":
            raise AssertionError("world residency key did not persist starter-a before fallback proof")

        fallback_before = read_metric_sum("chat_continuity_world_restore_fallback_total", ports=(39091, 39092))
        redis_del(world_key)
        first.close()

        second, resumed = resume_until_success("127.0.0.1", 36100, resume_token, user, logical_session_id)
        if resumed["world_id"] != "starter-a":
            raise AssertionError(f"world residency fallback did not land on safe default world: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8("should_fail_after_world_fallback"))
        error_code, error_message = second.wait_for_error(5.0)
        if error_code != ROOM_MISMATCH_ERRC or error_message != "room mismatch":
            raise AssertionError(
                f"world fallback did not reset room residency to lobby: code={error_code} message={error_message!r}"
            )

        fallback_after = read_metric_sum("chat_continuity_world_restore_fallback_total", ports=(39091, 39092))
        if fallback_after <= fallback_before:
            raise AssertionError(
                f"world restore fallback metric did not increase: before={fallback_before} after={fallback_after}"
            )

        print(
            "PASS world-residency-fallback: "
            f"logical_session_id={logical_session_id} world_id={resumed['world_id']} "
            f"fallback_delta={fallback_after - fallback_before:.0f}"
        )
    finally:
        first.close()
        second.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--scenario",
        choices=("gateway-restart", "server-restart", "locator-fallback", "world-residency-fallback", "both"),
        default="both",
    )
    args = parser.parse_args()

    try:
        if args.scenario in {"gateway-restart", "both"}:
            run_gateway_restart()
        if args.scenario in {"server-restart", "both"}:
            run_server_restart()
        if args.scenario in {"locator-fallback", "both"}:
            run_locator_fallback()
        if args.scenario in {"world-residency-fallback", "both"}:
            run_world_residency_fallback()
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
