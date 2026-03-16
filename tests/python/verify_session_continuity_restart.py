import argparse
import hashlib
import json
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

from session_continuity_common import ChatClient
from session_continuity_common import MSG_CHAT_SEND
from session_continuity_common import lp_utf8
from session_continuity_common import read_metric_sum
from session_continuity_common import read_metric_sum_labeled
from session_continuity_common import MSG_ERR
from stack_topology import ensure_stack_topology_artifacts
from stack_topology import first_server_for_other_world
from stack_topology import GENERATED_COMPOSE_PATH
from stack_topology import same_world_peer
from stack_topology import server_by_instance
from stack_topology import server_metrics_ports
from stack_topology import server_ready_ports

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPOSE_PROJECT_DIR = REPO_ROOT / "docker" / "stack"
COMPOSE_FILE = COMPOSE_PROJECT_DIR / "docker-compose.yml"
COMPOSE_ENV_FILE = COMPOSE_PROJECT_DIR / ".env.rudp-attach.example"
COMPOSE_PROJECT_NAME = "dynaxis-stack"
SESSION_DIRECTORY_PREFIX = "gateway/session/"
RESUME_LOCATOR_PREFIX = SESSION_DIRECTORY_PREFIX + "locator/"
CONTINUITY_WORLD_PREFIX = "dynaxis:continuity:world:"
CONTINUITY_WORLD_OWNER_PREFIX = "dynaxis:continuity:world-owner:"
ROOM_MISMATCH_ERRC = 0x0106
ADMIN_BASE_URL = "http://127.0.0.1:39200"

GATEWAY_READY_PORTS = {
    "gateway-1": 36001,
    "gateway-2": 36002,
}

TOPOLOGY = ensure_stack_topology_artifacts()
SERVER_READY_PORTS = server_ready_ports(TOPOLOGY)
SERVER_METRICS_PORTS = server_metrics_ports(TOPOLOGY)
GATEWAY_METRICS_PORTS = (36001, 36002)
PRIMARY_SERVER_ID = "server-1"
PRIMARY_SERVER = server_by_instance(TOPOLOGY, PRIMARY_SERVER_ID)
if PRIMARY_SERVER is None:
    raise RuntimeError("active topology does not define server-1")
PRIMARY_WORLD_ID = str(PRIMARY_SERVER["world_id"])
PRIMARY_WORLD_SHARD = str(PRIMARY_SERVER["shard"])
FALLBACK_SERVER = first_server_for_other_world(TOPOLOGY, PRIMARY_WORLD_ID)
SAME_WORLD_PEER = same_world_peer(TOPOLOGY, PRIMARY_SERVER_ID, PRIMARY_WORLD_ID)


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
        "-f",
        str(GENERATED_COMPOSE_PATH),
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


def redis_setex(key: str, ttl_sec: int, value: str) -> None:
    run_compose("exec", "-T", "redis", "redis-cli", "SETEX", key, str(ttl_sec), value)

def admin_request_json(path: str, method: str = "GET", body_obj=None):
    payload_bytes = None
    headers = {}
    if body_obj is not None:
        payload_bytes = json.dumps(body_obj).encode("utf-8")
        headers["Content-Type"] = "application/json"
        headers["Content-Length"] = str(len(payload_bytes))

    req = urllib.request.Request(
        f"{ADMIN_BASE_URL}{path}",
        method=method,
        headers=headers,
        data=payload_bytes,
    )
    try:
        with urllib.request.urlopen(req, timeout=5.0) as response:
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
    return status, payload


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


def read_world_restore_reason_metric(reason: str) -> float:
    return read_metric_sum_labeled(
        "chat_continuity_world_restore_fallback_reason_total",
        {"reason": reason},
        ports=SERVER_METRICS_PORTS,
    )


def require_fallback_server() -> dict:
    if FALLBACK_SERVER is None:
        raise RuntimeError(
            f"active topology does not define a fallback server outside world {PRIMARY_WORLD_ID}"
        )
    return FALLBACK_SERVER


def require_same_world_peer_server() -> dict:
    if SAME_WORLD_PEER is None:
        raise RuntimeError(
            f"active topology does not define a same-world peer for {PRIMARY_SERVER_ID} in {PRIMARY_WORLD_ID}"
        )
    return SAME_WORLD_PEER


def acquire_session_for_backend(target_backend: str, host: str, port: int, prefix: str) -> tuple[ChatClient, dict, str]:
    observed_backends: list[str] = []
    for attempt in range(1, 25):
        user = f"{prefix}_{int(time.time())}_{attempt}"
        client, login = connect_and_login(user, "", host, port)
        backend = current_backend_for_user(user)
        if backend == target_backend:
            return client, login, user
        if backend:
            observed_backends.append(backend)
        client.close()
    raise RuntimeError(
        f"failed to land on backend {target_backend}; observed={observed_backends}"
    )


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

    surviving_gateway_ports = (GATEWAY_READY_PORTS["gateway-2"],)
    hit_before = read_metric_sum("gateway_resume_routing_hit_total", ports=surviving_gateway_ports)
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

        hit_after = read_metric_sum("gateway_resume_routing_hit_total", ports=surviving_gateway_ports)
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

    first, login, user = acquire_session_for_backend(PRIMARY_SERVER_ID, gateway_host, gateway_port, "verify_resume_server")
    second = ChatClient(host=gateway_host, port=gateway_port)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        first.join_room(room, user)

        print(f"Restarting {PRIMARY_SERVER_ID} and waiting for continuity resume...")
        backend_before = wait_for_redis_value(f"{SESSION_DIRECTORY_PREFIX}{user}")
        alias_backend_before = wait_for_redis_value(f"{SESSION_DIRECTORY_PREFIX}{make_resume_routing_key(resume_token)}")
        if backend_before != PRIMARY_SERVER_ID:
            raise AssertionError(f"session was not attached to {PRIMARY_SERVER_ID} before restart: {backend_before}")
        if alias_backend_before != PRIMARY_SERVER_ID:
            raise AssertionError(f"resume alias was not attached to {PRIMARY_SERVER_ID} before restart: {alias_backend_before}")

        restart_service(PRIMARY_SERVER_ID)
        wait_ready(SERVER_READY_PORTS[PRIMARY_SERVER_ID])
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

    first, login, user = acquire_session_for_backend(PRIMARY_SERVER_ID, gateway_host, gateway_port, "verify_resume_locator")
    second = ChatClient(host="127.0.0.1", port=36100)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        routing_key = make_resume_routing_key(resume_token)
        alias_key = f"{SESSION_DIRECTORY_PREFIX}{routing_key}"
        locator_key = f"{RESUME_LOCATOR_PREFIX}{routing_key}"

        print("Dropping exact resume alias binding and forcing locator-based reconnect...")
        alias_backend_before = wait_for_redis_value(alias_key)
        locator_before = wait_for_redis_value(locator_key)
        locator_fields = parse_locator_payload(locator_before)
        if alias_backend_before != PRIMARY_SERVER_ID:
            raise AssertionError(f"resume alias was not attached to {PRIMARY_SERVER_ID} before locator fallback: {alias_backend_before}")
        if locator_fields.get("shard") != PRIMARY_WORLD_SHARD:
            raise AssertionError(f"resume locator hint did not capture shard boundary: {locator_before!r}")
        if locator_fields.get("world_id") != PRIMARY_WORLD_ID:
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
        if alias_backend_after != PRIMARY_SERVER_ID:
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

    first, login, user = acquire_session_for_backend(PRIMARY_SERVER_ID, gateway_host, gateway_port, "verify_resume_world")
    second = ChatClient(host="127.0.0.1", port=36100)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        world_key = f"{CONTINUITY_WORLD_PREFIX}{logical_session_id}"
        print("Dropping persisted world residency and forcing safe fallback...")
        if wait_for_redis_value(world_key) != PRIMARY_WORLD_ID:
            raise AssertionError(f"world residency key did not persist {PRIMARY_WORLD_ID} before fallback proof")

        fallback_before = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        missing_world_before = read_world_restore_reason_metric("missing_world")
        redis_del(world_key)
        first.close()

        second, resumed = resume_until_success("127.0.0.1", 36100, resume_token, user, logical_session_id)
        if resumed["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"world residency fallback did not land on safe default world: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8("should_fail_after_world_fallback"))
        error_code, error_message = second.wait_for_error(5.0)
        if error_code != ROOM_MISMATCH_ERRC or error_message != "room mismatch":
            raise AssertionError(
                f"world fallback did not reset room residency to lobby: code={error_code} message={error_message!r}"
            )

        fallback_after = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        missing_world_after = read_world_restore_reason_metric("missing_world")
        if fallback_after <= fallback_before:
            raise AssertionError(
                f"world restore fallback metric did not increase: before={fallback_before} after={fallback_after}"
            )
        if missing_world_after <= missing_world_before:
            raise AssertionError(
                "world restore missing_world reason metric did not increase: "
                f"before={missing_world_before} after={missing_world_after}"
            )

        print(
            "PASS world-residency-fallback: "
            f"logical_session_id={logical_session_id} world_id={resumed['world_id']} "
            f"fallback_delta={fallback_after - fallback_before:.0f} "
            f"reason_delta={missing_world_after - missing_world_before:.0f}"
        )
    finally:
        first.close()
        second.close()


def run_world_owner_fallback() -> None:
    room = f"resume_world_owner_room_{int(time.time())}"
    gateway_host = "127.0.0.1"
    gateway_port = 36101

    fallback_server = require_fallback_server()
    first, login, user = acquire_session_for_backend(PRIMARY_SERVER_ID, gateway_host, gateway_port, "verify_resume_world_owner")
    second = ChatClient(host="127.0.0.1", port=36100)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        world_owner_key = f"{CONTINUITY_WORLD_OWNER_PREFIX}{login['world_id']}"
        print("Overwriting persisted world owner and forcing safe fallback...")
        if wait_for_redis_value(world_owner_key) != PRIMARY_SERVER_ID:
            raise AssertionError(f"world owner key did not persist {PRIMARY_SERVER_ID} before fallback proof")

        world_fallback_before = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_fallback_before = read_metric_sum(
            "chat_continuity_world_owner_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_mismatch_before = read_world_restore_reason_metric("owner_mismatch")
        redis_setex(world_owner_key, 900, str(fallback_server["instance_id"]))
        if wait_for_redis_value(world_owner_key) != str(fallback_server["instance_id"]):
            raise AssertionError("world owner key did not update to mismatched owner before fallback proof")
        first.close()

        second, resumed = resume_until_success("127.0.0.1", 36100, resume_token, user, logical_session_id)
        if resumed["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"world owner fallback did not land on safe default world: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8("should_fail_after_world_owner_fallback"))
        error_code, error_message = second.wait_for_error(5.0)
        if error_code != ROOM_MISMATCH_ERRC or error_message != "room mismatch":
            raise AssertionError(
                f"world owner fallback did not reset room residency to lobby: code={error_code} message={error_message!r}"
            )

        world_fallback_after = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_fallback_after = read_metric_sum(
            "chat_continuity_world_owner_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_mismatch_after = read_world_restore_reason_metric("owner_mismatch")
        if world_fallback_after <= world_fallback_before:
            raise AssertionError(
                "world restore fallback metric did not increase during owner mismatch fallback: "
                f"before={world_fallback_before} after={world_fallback_after}"
            )
        if owner_fallback_after <= owner_fallback_before:
            raise AssertionError(
                "world owner restore fallback metric did not increase: "
                f"before={owner_fallback_before} after={owner_fallback_after}"
            )
        if owner_mismatch_after <= owner_mismatch_before:
            raise AssertionError(
                "world restore owner_mismatch reason metric did not increase: "
                f"before={owner_mismatch_before} after={owner_mismatch_after}"
            )

        world_owner_after = wait_for_redis_value(world_owner_key)
        if world_owner_after != PRIMARY_SERVER_ID:
            raise AssertionError(
                f"world owner key was not rewritten to the current backend owner: {world_owner_after!r}"
            )

        print(
            "PASS world-owner-fallback: "
            f"logical_session_id={logical_session_id} world_id={resumed['world_id']} "
            f"world_fallback_delta={world_fallback_after - world_fallback_before:.0f} "
            f"owner_fallback_delta={owner_fallback_after - owner_fallback_before:.0f} "
            f"reason_delta={owner_mismatch_after - owner_mismatch_before:.0f}"
        )
    finally:
        first.close()
        second.close()


def run_world_owner_missing_fallback() -> None:
    room = f"resume_world_owner_missing_room_{int(time.time())}"
    gateway_host = "127.0.0.1"
    gateway_port = 36101

    first, login, user = acquire_session_for_backend(
        PRIMARY_SERVER_ID,
        gateway_host,
        gateway_port,
        "verify_resume_world_owner_missing",
    )
    second = ChatClient(host="127.0.0.1", port=36100)
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        world_owner_key = f"{CONTINUITY_WORLD_OWNER_PREFIX}{login['world_id']}"
        print("Deleting persisted world owner and forcing safe fallback...")
        if wait_for_redis_value(world_owner_key) != PRIMARY_SERVER_ID:
            raise AssertionError(f"world owner key did not persist {PRIMARY_SERVER_ID} before missing-owner proof")

        world_fallback_before = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_fallback_before = read_metric_sum(
            "chat_continuity_world_owner_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        missing_owner_before = read_world_restore_reason_metric("missing_owner")
        redis_del(world_owner_key)
        if redis_get(world_owner_key):
            raise AssertionError("world owner key still exists after delete")
        first.close()

        second, resumed = resume_until_success("127.0.0.1", 36100, resume_token, user, logical_session_id)
        if resumed["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"missing-owner fallback did not land on safe default world: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8("should_fail_after_world_owner_missing_fallback"))
        error_code, error_message = second.wait_for_error(5.0)
        if error_code != ROOM_MISMATCH_ERRC or error_message != "room mismatch":
            raise AssertionError(
                "missing-owner fallback did not reset room residency to lobby: "
                f"code={error_code} message={error_message!r}"
            )

        world_fallback_after = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_fallback_after = read_metric_sum(
            "chat_continuity_world_owner_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        missing_owner_after = read_world_restore_reason_metric("missing_owner")
        if world_fallback_after <= world_fallback_before:
            raise AssertionError(
                "world restore fallback metric did not increase during missing-owner fallback: "
                f"before={world_fallback_before} after={world_fallback_after}"
            )
        if owner_fallback_after <= owner_fallback_before:
            raise AssertionError(
                "world owner restore fallback metric did not increase during missing-owner fallback: "
                f"before={owner_fallback_before} after={owner_fallback_after}"
            )
        if missing_owner_after <= missing_owner_before:
            raise AssertionError(
                "world restore missing_owner reason metric did not increase: "
                f"before={missing_owner_before} after={missing_owner_after}"
            )

        world_owner_after = wait_for_redis_value(world_owner_key)
        if world_owner_after != PRIMARY_SERVER_ID:
            raise AssertionError(
                f"world owner key was not rewritten after missing-owner fallback: {world_owner_after!r}"
            )

        print(
            "PASS world-owner-missing-fallback: "
            f"logical_session_id={logical_session_id} world_id={resumed['world_id']} "
            f"world_fallback_delta={world_fallback_after - world_fallback_before:.0f} "
            f"owner_fallback_delta={owner_fallback_after - owner_fallback_before:.0f} "
            f"reason_delta={missing_owner_after - missing_owner_before:.0f}"
        )
    finally:
        first.close()
        second.close()


def run_world_drain_fallback() -> None:
    room = f"resume_world_drain_room_{int(time.time())}"
    gateway_host = "127.0.0.1"
    gateway_port = 36101
    fallback_server = require_fallback_server()
    fallback_server_id = str(fallback_server["instance_id"])
    fallback_world_id = str(fallback_server["world_id"])

    first, login, user = acquire_session_for_backend(PRIMARY_SERVER_ID, gateway_host, gateway_port, "verify_world_drain")
    second = ChatClient(host="127.0.0.1", port=36100)
    fresh = None
    world_policy_path = ""
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        routing_key = make_resume_routing_key(resume_token)
        alias_key = f"{SESSION_DIRECTORY_PREFIX}{routing_key}"
        world_policy_path = f"/api/v1/worlds/{login['world_id']}/policy"

        print("Declaring world drain policy and forcing fresh admission/resume away from draining owner...")
        if wait_for_redis_value(alias_key) != PRIMARY_SERVER_ID:
            raise AssertionError(f"resume alias was not attached to {PRIMARY_SERVER_ID} before drain proof")

        status, payload = admin_request_json(
            world_policy_path,
            method="PUT",
            body_obj={
                "draining": True,
                "replacement_owner_instance_id": None,
            },
        )
        if status != 200:
            raise AssertionError(f"world policy PUT failed: status={status} payload={payload}")
        policy_payload = (payload or {}).get("data", {}).get("policy", {})
        if policy_payload.get("draining") is not True or policy_payload.get("replacement_owner_instance_id") is not None:
            raise AssertionError(f"world policy PUT did not persist expected state: {payload}")

        fallback_before = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_fallback_before = read_metric_sum(
            "chat_continuity_world_owner_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        drain_reason_before = read_world_restore_reason_metric("draining_replacement_unhonored")
        sticky_filtered_before = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "sticky"},
            ports=GATEWAY_METRICS_PORTS,
        )
        candidate_filtered_before = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "candidate"},
            ports=GATEWAY_METRICS_PORTS,
        )
        replacement_selected_before = read_metric_sum(
            "gateway_world_policy_replacement_selected_total",
            ports=GATEWAY_METRICS_PORTS,
        )

        fresh_user = f"verify_world_drain_fresh_{int(time.time())}"
        fresh, fresh_login = connect_and_login(fresh_user, "", "127.0.0.1", 36100)
        fresh_backend = current_backend_for_user(fresh_user)
        if fresh_backend != fallback_server_id:
            raise AssertionError(
                f"fresh admission did not avoid draining owner: expected {fallback_server_id}, got {fresh_backend!r}"
            )
        if fresh_login["world_id"] != fallback_world_id:
            raise AssertionError(f"fresh admission did not land on fallback world {fallback_world_id}: {fresh_login}")
        fresh.close()
        fresh = None

        first.close()
        second, resumed = resume_until_success("127.0.0.1", 36100, resume_token, user, logical_session_id)
        resumed_backend = current_backend_for_resume_token(resume_token)
        if resumed_backend != fallback_server_id:
            raise AssertionError(
                f"resume routing did not rebind away from the draining owner: {resumed_backend!r}"
            )
        if resumed["world_id"] != fallback_world_id:
            raise AssertionError(f"resume did not fall back to world {fallback_world_id}: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8("should_fail_after_world_drain_fallback"))
        error_code, error_message = second.wait_for_error(5.0)
        if error_code != ROOM_MISMATCH_ERRC or error_message != "room mismatch":
            raise AssertionError(
                f"world drain fallback did not reset room residency to lobby: code={error_code} message={error_message!r}"
            )

        fallback_after = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        owner_fallback_after = read_metric_sum(
            "chat_continuity_world_owner_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        drain_reason_after = read_world_restore_reason_metric("draining_replacement_unhonored")
        sticky_filtered_after = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "sticky"},
            ports=GATEWAY_METRICS_PORTS,
        )
        candidate_filtered_after = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "candidate"},
            ports=GATEWAY_METRICS_PORTS,
        )
        replacement_selected_after = read_metric_sum(
            "gateway_world_policy_replacement_selected_total",
            ports=GATEWAY_METRICS_PORTS,
        )
        if fallback_after <= fallback_before:
            raise AssertionError(
                f"world drain restore fallback metric did not increase: before={fallback_before} after={fallback_after}"
            )
        if owner_fallback_after != owner_fallback_before:
            raise AssertionError(
                "world owner fallback aggregate changed during drain fallback: "
                f"before={owner_fallback_before} after={owner_fallback_after}"
            )
        if drain_reason_after <= drain_reason_before:
            raise AssertionError(
                "draining_replacement_unhonored reason metric did not increase: "
                f"before={drain_reason_before} after={drain_reason_after}"
            )
        if sticky_filtered_after <= sticky_filtered_before:
            raise AssertionError(
                "gateway sticky world-policy filter metric did not increase during drain resume: "
                f"before={sticky_filtered_before} after={sticky_filtered_after}"
            )
        if candidate_filtered_after <= candidate_filtered_before:
            raise AssertionError(
                "gateway candidate world-policy filter metric did not increase during drain routing: "
                f"before={candidate_filtered_before} after={candidate_filtered_after}"
            )
        if replacement_selected_after != replacement_selected_before:
            raise AssertionError(
                "gateway replacement-selected metric changed without a declared replacement owner: "
                f"before={replacement_selected_before} after={replacement_selected_after}"
            )

        alias_backend_after = wait_for_redis_value(alias_key)
        if alias_backend_after != fallback_server_id:
            raise AssertionError(
                f"resume alias was not rewritten to the replacement backend: {alias_backend_after!r}"
            )

        print(
            "PASS world-drain-fallback: "
            f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} "
            f"fallback_delta={fallback_after - fallback_before:.0f} "
            f"reason_delta={drain_reason_after - drain_reason_before:.0f} "
            f"sticky_delta={sticky_filtered_after - sticky_filtered_before:.0f} "
            f"candidate_delta={candidate_filtered_after - candidate_filtered_before:.0f}"
        )
    finally:
        if world_policy_path:
            status, payload = admin_request_json(world_policy_path, method="DELETE")
            if status not in (0, 200):
                raise AssertionError(f"world policy DELETE failed: status={status} payload={payload}")
        first.close()
        second.close()
        if fresh is not None:
            fresh.close()


def run_world_drain_reassignment() -> None:
    room = f"resume_world_reassignment_room_{int(time.time())}"
    message = f"resume_world_reassignment_msg_{int(time.time() * 1000)}"
    gateway_host = "127.0.0.1"
    gateway_port = 36101
    replacement_server = require_same_world_peer_server()
    replacement_server_id = str(replacement_server["instance_id"])
    world_owner_key = f"{CONTINUITY_WORLD_OWNER_PREFIX}{PRIMARY_WORLD_ID}"

    first, login, user = acquire_session_for_backend(PRIMARY_SERVER_ID, gateway_host, gateway_port, "verify_world_reassignment")
    second = ChatClient(host="127.0.0.1", port=36100)
    fresh = None
    world_policy_path = ""
    try:
        logical_session_id, resume_token = assert_initial_login(login, user)
        if login["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"unexpected initial world residency: {login}")
        first.join_room(room, user)

        routing_key = make_resume_routing_key(resume_token)
        alias_key = f"{SESSION_DIRECTORY_PREFIX}{routing_key}"
        world_policy_path = f"/api/v1/worlds/{login['world_id']}/policy"

        print("Declaring world drain policy and proving honored same-world replacement...")
        if wait_for_redis_value(alias_key) != PRIMARY_SERVER_ID:
            raise AssertionError(f"resume alias was not attached to {PRIMARY_SERVER_ID} before reassignment proof")

        status, payload = admin_request_json(
            world_policy_path,
            method="PUT",
            body_obj={
                "draining": True,
                "replacement_owner_instance_id": replacement_server_id,
            },
        )
        if status != 200:
            raise AssertionError(f"world policy PUT failed: status={status} payload={payload}")
        policy_payload = (payload or {}).get("data", {}).get("policy", {})
        if (
            policy_payload.get("draining") is not True
            or policy_payload.get("replacement_owner_instance_id") != replacement_server_id
        ):
            raise AssertionError(f"world policy PUT did not persist reassignment target: {payload}")

        world_restore_before = read_metric_sum(
            "chat_continuity_world_restore_total",
            ports=SERVER_METRICS_PORTS,
        )
        world_owner_restore_before = read_metric_sum(
            "chat_continuity_world_owner_restore_total",
            ports=SERVER_METRICS_PORTS,
        )
        fallback_before = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        drain_reason_before = read_world_restore_reason_metric("draining_replacement_unhonored")
        sticky_filtered_before = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "sticky"},
            ports=GATEWAY_METRICS_PORTS,
        )
        candidate_filtered_before = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "candidate"},
            ports=GATEWAY_METRICS_PORTS,
        )
        replacement_selected_before = read_metric_sum(
            "gateway_world_policy_replacement_selected_total",
            ports=GATEWAY_METRICS_PORTS,
        )

        fresh, fresh_login, fresh_user = acquire_session_for_backend(
            replacement_server_id,
            "127.0.0.1",
            36100,
            "verify_world_reassignment_fresh",
        )
        fresh_backend = current_backend_for_user(fresh_user)
        if fresh_backend != replacement_server_id:
            raise AssertionError(
                f"fresh admission did not reach replacement backend {replacement_server_id}: {fresh_backend!r}"
            )
        if fresh_login["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"fresh admission did not preserve same-world residency: {fresh_login}")
        if wait_for_redis_value(world_owner_key) != replacement_server_id:
            raise AssertionError(
                f"world owner key was not reasserted to replacement backend {replacement_server_id}"
            )
        fresh.close()
        fresh = None

        first.close()
        second, resumed = resume_until_success("127.0.0.1", 36100, resume_token, user, logical_session_id)
        resumed_backend = current_backend_for_resume_token(resume_token)
        if resumed_backend != replacement_server_id:
            raise AssertionError(
                f"resume routing did not bind to the replacement backend: {resumed_backend!r}"
            )
        if resumed["world_id"] != PRIMARY_WORLD_ID:
            raise AssertionError(f"resume did not preserve same-world residency: {resumed}")

        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        world_restore_after = read_metric_sum(
            "chat_continuity_world_restore_total",
            ports=SERVER_METRICS_PORTS,
        )
        world_owner_restore_after = read_metric_sum(
            "chat_continuity_world_owner_restore_total",
            ports=SERVER_METRICS_PORTS,
        )
        fallback_after = read_metric_sum(
            "chat_continuity_world_restore_fallback_total",
            ports=SERVER_METRICS_PORTS,
        )
        drain_reason_after = read_world_restore_reason_metric("draining_replacement_unhonored")
        sticky_filtered_after = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "sticky"},
            ports=GATEWAY_METRICS_PORTS,
        )
        candidate_filtered_after = read_metric_sum_labeled(
            "gateway_world_policy_filtered_total",
            {"source": "candidate"},
            ports=GATEWAY_METRICS_PORTS,
        )
        replacement_selected_after = read_metric_sum(
            "gateway_world_policy_replacement_selected_total",
            ports=GATEWAY_METRICS_PORTS,
        )

        if world_restore_after <= world_restore_before:
            raise AssertionError(
                f"world restore success counter did not increase: before={world_restore_before} after={world_restore_after}"
            )
        if world_owner_restore_after <= world_owner_restore_before:
            raise AssertionError(
                "world owner restore success counter did not increase: "
                f"before={world_owner_restore_before} after={world_owner_restore_after}"
            )
        if fallback_after != fallback_before:
            raise AssertionError(
                f"world restore fallback counter changed during honored reassignment: before={fallback_before} after={fallback_after}"
            )
        if drain_reason_after != drain_reason_before:
            raise AssertionError(
                "draining_replacement_unhonored reason changed during honored reassignment: "
                f"before={drain_reason_before} after={drain_reason_after}"
            )
        if sticky_filtered_after <= sticky_filtered_before:
            raise AssertionError(
                "gateway sticky world-policy filter metric did not increase during reassignment resume: "
                f"before={sticky_filtered_before} after={sticky_filtered_after}"
            )
        if candidate_filtered_after <= candidate_filtered_before:
            raise AssertionError(
                "gateway candidate world-policy filter metric did not increase during reassignment routing: "
                f"before={candidate_filtered_before} after={candidate_filtered_after}"
            )
        if replacement_selected_after <= replacement_selected_before:
            raise AssertionError(
                "gateway replacement-selected metric did not increase during honored reassignment: "
                f"before={replacement_selected_before} after={replacement_selected_after}"
            )

        alias_backend_after = wait_for_redis_value(alias_key)
        if alias_backend_after != replacement_server_id:
            raise AssertionError(
                f"resume alias was not rewritten to the replacement backend: {alias_backend_after!r}"
            )
        if wait_for_redis_value(world_owner_key) != replacement_server_id:
            raise AssertionError(
                f"world owner key drifted after reassignment restore: expected {replacement_server_id}"
            )

        print(
            "PASS world-drain-reassignment: "
            f"logical_session_id={logical_session_id} resumed_backend={resumed_backend} "
            f"world_restore_delta={world_restore_after - world_restore_before:.0f} "
            f"owner_restore_delta={world_owner_restore_after - world_owner_restore_before:.0f} "
            f"replacement_selected_delta={replacement_selected_after - replacement_selected_before:.0f}"
        )
    finally:
        if world_policy_path:
            status, payload = admin_request_json(world_policy_path, method="DELETE")
            if status not in (0, 200):
                raise AssertionError(f"world policy DELETE failed: status={status} payload={payload}")
        first.close()
        second.close()
        if fresh is not None:
            fresh.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--scenario",
        choices=(
            "gateway-restart",
            "server-restart",
            "locator-fallback",
            "world-residency-fallback",
            "world-owner-fallback",
            "world-owner-missing-fallback",
            "world-drain-fallback",
            "world-drain-reassignment",
            "both",
        ),
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
        if args.scenario in {"world-owner-fallback", "both"}:
            run_world_owner_fallback()
        if args.scenario in {"world-owner-missing-fallback", "both"}:
            run_world_owner_missing_fallback()
        if args.scenario in {"world-drain-fallback", "both"}:
            run_world_drain_fallback()
        if args.scenario == "world-drain-reassignment":
            run_world_drain_reassignment()
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
