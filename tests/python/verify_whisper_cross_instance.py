import socket
import struct
import subprocess
import sys
import time
from typing import Optional

HOST = "127.0.0.1"
PORT = 6000  # HAProxy frontend

MSG_LOGIN_REQ = 0x0010
MSG_LOGIN_RES = 0x0011
MSG_WHISPER_REQ = 0x0104
MSG_WHISPER_RES = 0x0105
MSG_WHISPER_BROADCAST = 0x0106


def lp_utf8(value: str) -> bytes:
    raw = value.encode("utf-8")
    return struct.pack(">H", len(raw)) + raw


def read_exact(sock: socket.socket, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise ConnectionError("socket closed while reading")
        out.extend(chunk)
    return bytes(out)


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
        ts = int(time.time() * 1000) & 0xFFFFFFFF
        header = struct.pack(">HHHII", len(payload), msg_id, flags, self.seq, ts)
        self.seq += 1
        self.sock.sendall(header + payload)

    def recv_frame(self, timeout_sec: float):
        self.sock.settimeout(timeout_sec)
        header = read_exact(self.sock, 14)
        length, msg_id, flags, seq, ts = struct.unpack(">HHHII", header)
        payload = read_exact(self.sock, length) if length else b""
        return msg_id, flags, seq, ts, payload

    def wait_for(self, accepted_ids, timeout_sec: float):
        deadline = time.monotonic() + timeout_sec
        seen = []
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return None, seen
            try:
                msg_id, _, _, _, payload = self.recv_frame(remain)
            except socket.timeout:
                return None, seen
            seen.append(msg_id)
            if msg_id in accepted_ids:
                return (msg_id, payload), seen

    def login(self) -> bool:
        payload = lp_utf8(self.name) + lp_utf8("")
        self.send_frame(MSG_LOGIN_REQ, payload)
        match, seen = self.wait_for({MSG_LOGIN_RES}, 4.0)
        if match is None:
            print(f"[{self.name}] login response not received; seen={seen}")
            return False
        return True

    def send_whisper(self, target_user: str, text: str) -> None:
        payload = lp_utf8(target_user) + lp_utf8(text)
        self.send_frame(MSG_WHISPER_REQ, payload)


def redis_get_session_backend(user_name: str):
    key = f"gateway/session/{user_name}"
    cmd = [
        "docker",
        "exec",
        "knights-stack-redis-1",
        "redis-cli",
        "--raw",
        "GET",
        key,
    ]
    try:
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT).strip()
    except subprocess.CalledProcessError as e:
        print(f"redis-cli failed for {key}: {e.output}")
        return None
    if not out or out == "(nil)":
        return None
    return out


def resolve_backend_with_retry(user_name: str, retries: int = 60, sleep_sec: float = 0.1) -> Optional[str]:
    for _ in range(retries):
        backend = redis_get_session_backend(user_name)
        if backend:
            return backend
        time.sleep(sleep_sec)
    return None


def main() -> int:
    sender_name = f"ws_sender_{int(time.time())}"
    sender = ChatClient(sender_name)
    recipient = None
    same_backend_holders: list[ChatClient] = []

    try:
        sender.connect()
        if not sender.login():
            return 1

        sender_backend = resolve_backend_with_retry(sender_name)
        if not sender_backend:
            print("Failed to resolve sender backend from Redis session directory")
            return 1

        recipient_backend = None
        recipient_name = None

        # Retry recipient login until it lands on a different backend.
        # Keep 일부 same-backend 연결을 잠시 유지해 least-connections 관측치가 갱신될 시간을 줍니다.
        max_attempts = 24
        attempt_delay_sec = 0.75
        max_holders = 3
        for attempt in range(max_attempts):

            candidate = f"ws_recipient_{int(time.time())}_{attempt}"
            r = ChatClient(candidate)
            r.connect()
            if not r.login():
                r.close()
                time.sleep(attempt_delay_sec)
                continue

            backend = resolve_backend_with_retry(candidate)

            if backend and backend != sender_backend:
                recipient = r
                recipient_name = candidate
                recipient_backend = backend
                break

            # 같은 backend에 배치된 연결을 짧게 유지해 라우팅 편향을 완화합니다.
            if backend == sender_backend:
                same_backend_holders.append(r)
                if len(same_backend_holders) > max_holders:
                    old = same_backend_holders.pop(0)
                    old.close()
            else:
                r.close()

            time.sleep(attempt_delay_sec)

        if recipient is None or recipient_name is None or recipient_backend is None:
            print("Could not place sender/recipient on different server backends")
            return 1

        print(
            "Selected backends: "
            f"sender={sender_name}->{sender_backend}, "
            f"recipient={recipient_name}->{recipient_backend}"
        )

        text = f"cross-instance whisper {int(time.time())}"
        sender.send_whisper(recipient_name, text)

        recv_match, recv_seen = recipient.wait_for({MSG_WHISPER_BROADCAST}, 5.0)
        if recv_match is None:
            print(f"Recipient did not receive whisper broadcast; seen={recv_seen}")
            return 1

        sender_match, sender_seen = sender.wait_for({MSG_WHISPER_RES, MSG_WHISPER_BROADCAST}, 5.0)
        if sender_match is None:
            print(f"Sender did not receive whisper ack/echo; seen={sender_seen}")
            return 1

        print("PASS: cross-instance whisper delivered")
        print(f"Recipient seen opcodes: {recv_seen}")
        print(f"Sender seen opcodes: {sender_seen}")
        return 0

    except Exception as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        for holder in same_backend_holders:
            holder.close()
        if recipient is not None:
            recipient.close()
        sender.close()


if __name__ == "__main__":
    raise SystemExit(main())
