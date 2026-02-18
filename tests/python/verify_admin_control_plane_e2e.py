import json
import socket
import struct
import time
import urllib.error
import urllib.parse
import urllib.request


HOST = "127.0.0.1"
PORT = 6000
ADMIN_BASE = "http://127.0.0.1:39200"

MSG_PONG = 0x0003
MSG_LOGIN_REQ = 0x0010
MSG_LOGIN_RES = 0x0011
MSG_JOIN_ROOM = 0x0102
MSG_CHAT_BROADCAST = 0x0101


def lp_utf8(value: str) -> bytes:
    raw = value.encode("utf-8")
    return struct.pack(">H", len(raw)) + raw


def read_exact(sock: socket.socket, size: int) -> bytes:
    out = bytearray()
    while len(out) < size:
        chunk = sock.recv(size - len(out))
        if not chunk:
            raise ConnectionError("socket closed while reading")
        out.extend(chunk)
    return bytes(out)


def request_json(path: str, method: str = "GET"):
    req = urllib.request.Request(f"{ADMIN_BASE}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            status = resp.status
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status = exc.code
        body = exc.read().decode("utf-8", errors="replace")

    payload = None
    if body:
        try:
            payload = json.loads(body)
        except json.JSONDecodeError:
            payload = None
    return status, payload, body


def wait_ready(path: str, timeout_sec: float = 30.0) -> None:
    deadline = time.time() + timeout_sec
    last_error = "unknown"
    while time.time() < deadline:
        try:
            status, _, _ = request_json(path, method="GET")
            if status == 200:
                return
            last_error = f"status={status}"
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for {path}: {last_error}")


class ChatClient:
    def __init__(self, name: str):
        self.name = name
        self.seq = 1
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    def connect(self) -> None:
        self.sock.connect((HOST, PORT))

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:
            pass

    def send_frame(self, msg_id: int, payload: bytes = b"", flags: int = 0) -> None:
        ts_ms = int(time.time() * 1000) & 0xFFFFFFFF
        header = struct.pack(">HHHII", len(payload), msg_id, flags, self.seq, ts_ms)
        self.seq += 1
        self.sock.sendall(header + payload)

    def recv_frame(self, timeout_sec: float):
        self.sock.settimeout(timeout_sec)
        header = read_exact(self.sock, 14)
        length, msg_id, _flags, _seq, _ts = struct.unpack(">HHHII", header)
        payload = read_exact(self.sock, length) if length else b""
        return msg_id, payload

    def login(self) -> bool:
        payload = lp_utf8(self.name) + lp_utf8("")
        self.send_frame(MSG_LOGIN_REQ, payload)
        match, seen = self.wait_for({MSG_LOGIN_RES}, timeout_sec=4.0)
        if match is None:
            print(f"[{self.name}] login response not received; seen={seen}")
            return False
        return True

    def join(self, room: str) -> None:
        self.send_frame(MSG_JOIN_ROOM, lp_utf8(room) + lp_utf8(""))

    def wait_for(self, accepted_ids: set[int], timeout_sec: float):
        deadline = time.monotonic() + timeout_sec
        seen = []
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return None, seen
            try:
                msg_id, payload = self.recv_frame(remain)
            except socket.timeout:
                return None, seen
            seen.append(msg_id)
            if msg_id in accepted_ids:
                return (msg_id, payload), seen

    def drain(self, timeout_sec: float):
        seen = []
        deadline = time.monotonic() + timeout_sec
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return seen
            try:
                msg_id, _payload = self.recv_frame(min(0.2, remain))
                seen.append(msg_id)
            except socket.timeout:
                continue


def wait_for_announcement(client: ChatClient, text: str, timeout_sec: float):
    expected = text.encode("utf-8")
    deadline = time.monotonic() + timeout_sec
    seen = []
    while True:
        remain = deadline - time.monotonic()
        if remain <= 0:
            return False, seen
        try:
            msg_id, payload = client.recv_frame(remain)
        except socket.timeout:
            return False, seen

        seen.append(msg_id)
        if msg_id == MSG_CHAT_BROADCAST and expected in payload:
            return True, seen


def wait_disconnected(client: ChatClient, timeout_sec: float) -> bool:
    deadline = time.monotonic() + timeout_sec
    client.sock.settimeout(0.5)
    while time.monotonic() < deadline:
        try:
            data = client.sock.recv(4096)
            if data == b"":
                return True
        except socket.timeout:
            continue
        except (ConnectionResetError, OSError):
            return True
    return False


def expect_accepted(path: str, method: str):
    status, payload, body = request_json(path, method=method)
    if status != 200:
        raise RuntimeError(f"{method} {path} expected 200, got {status}: {body}")
    data = (payload or {}).get("data", {})
    if data.get("accepted") is not True:
        raise RuntimeError(f"{method} {path} missing accepted=true")
    return payload


def main() -> int:
    stamp = int(time.time())
    room = f"admin-e2e-room-{stamp}"
    user_a = f"admin_e2e_a_{stamp}"
    user_b = f"admin_e2e_b_{stamp}"
    announce_text = f"admin-e2e-announcement-{stamp}"

    c1 = ChatClient(user_a)
    c2 = ChatClient(user_b)

    try:
        wait_ready("/healthz")
        wait_ready("/readyz")

        status, payload, body = request_json("/api/v1/auth/context")
        if status != 200:
            raise RuntimeError(f"auth context request failed: {status} {body}")
        auth_data = (payload or {}).get("data", {})
        if auth_data.get("mode") != "off":
            raise RuntimeError("expected ADMIN_AUTH_MODE=off in docker stack")

        c1.connect()
        c2.connect()
        if not c1.login() or not c2.login():
            raise RuntimeError("login failed for one or more clients")

        c1.join(room)
        c2.join(room)

        c1_seen = c1.drain(1.0)
        c2_seen = c2.drain(1.0)
        print(f"drain {user_a} seen={c1_seen}")
        print(f"drain {user_b} seen={c2_seen}")

        announce_path = (
            "/api/v1/announcements?text="
            + urllib.parse.quote(announce_text, safe="")
            + "&priority=warn"
        )
        expect_accepted(announce_path, "POST")

        ok1, seen1 = wait_for_announcement(c1, announce_text, timeout_sec=5.0)
        ok2, seen2 = wait_for_announcement(c2, announce_text, timeout_sec=5.0)
        if not ok1 or not ok2:
            raise RuntimeError(
                f"announcement not observed on both clients (seen1={seen1}, seen2={seen2})"
            )
        print(f"announcement observed on both clients: seen1={seen1}, seen2={seen2}")

        disconnect_path = (
            "/api/v1/users/disconnect?client_id="
            + urllib.parse.quote(user_a, safe="")
            + "&reason="
            + urllib.parse.quote("admin-e2e-disconnect", safe="")
        )
        expect_accepted(disconnect_path, "POST")

        if not wait_disconnected(c1, timeout_sec=6.0):
            raise RuntimeError("target client did not disconnect within timeout")
        print("target client disconnected")

        if wait_disconnected(c2, timeout_sec=1.0):
            raise RuntimeError("non-target client disconnected unexpectedly")

        c2.send_frame(MSG_PONG)

        expect_accepted("/api/v1/settings?key=recent_history_limit&value=35", "PATCH")

        print("PASS: admin control-plane E2E smoke")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        c1.close()
        c2.close()


if __name__ == "__main__":
    raise SystemExit(main())
