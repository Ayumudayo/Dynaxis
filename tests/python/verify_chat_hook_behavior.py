import socket
import time


HOST = "127.0.0.1"
PORT = 6000

MSG_LOGIN_REQ = 0x0010
MSG_LOGIN_RES = 0x0011
MSG_CHAT_SEND = 0x0100
MSG_CHAT_BROADCAST = 0x0101
MSG_JOIN_ROOM = 0x0102
FLAG_SELF = 0x0100


def lp_utf8(value: str) -> bytes:
    raw = value.encode("utf-8")
    return len(raw).to_bytes(2, byteorder="big", signed=False) + raw


def read_exact(sock: socket.socket, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise ConnectionError("socket closed while reading")
        out.extend(chunk)
    return bytes(out)


class ChatClient:
    def __init__(self) -> None:
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
        header = (
            len(payload).to_bytes(2, byteorder="big", signed=False)
            + msg_id.to_bytes(2, byteorder="big", signed=False)
            + flags.to_bytes(2, byteorder="big", signed=False)
            + self.seq.to_bytes(4, byteorder="big", signed=False)
            + ts.to_bytes(4, byteorder="big", signed=False)
        )
        self.seq += 1
        self.sock.sendall(header + payload)

    def recv_frame(self, timeout_sec: float) -> tuple[int, int, bytes]:
        self.sock.settimeout(timeout_sec)
        header = read_exact(self.sock, 14)
        length = int.from_bytes(header[0:2], byteorder="big", signed=False)
        msg_id = int.from_bytes(header[2:4], byteorder="big", signed=False)
        flags = int.from_bytes(header[4:6], byteorder="big", signed=False)
        payload = read_exact(self.sock, length) if length else b""
        return msg_id, flags, payload

    def wait_for_login(self, timeout_sec: float) -> bool:
        deadline = time.monotonic() + timeout_sec
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return False
            try:
                msg_id, _flags, _payload = self.recv_frame(remain)
            except socket.timeout:
                return False
            if msg_id == MSG_LOGIN_RES:
                return True

    def wait_for_self_broadcast_contains(self, token: str, timeout_sec: float) -> bool:
        expected = token.encode("utf-8")
        deadline = time.monotonic() + timeout_sec
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return False
            try:
                msg_id, flags, payload = self.recv_frame(remain)
            except socket.timeout:
                return False
            if (
                msg_id == MSG_CHAT_BROADCAST
                and (flags & FLAG_SELF) != 0
                and expected in payload
            ):
                return True


def main() -> int:
    user = f"verify_hook_{int(time.time())}"
    shout = f"hook_probe_{int(time.time() * 1000)}"
    command = f"/shout {shout}"
    expected = shout.upper()

    client = ChatClient()
    try:
        client.connect()
        client.send_frame(MSG_LOGIN_REQ, lp_utf8(user) + lp_utf8(""))
        if not client.wait_for_login(5.0):
            print("FAIL: login response was not received")
            return 1

        client.send_frame(MSG_JOIN_ROOM, lp_utf8("lobby") + lp_utf8(""))
        client.send_frame(MSG_CHAT_SEND, lp_utf8("lobby") + lp_utf8(command))

        if not client.wait_for_self_broadcast_contains(expected, 5.0):
            print(
                f"FAIL: hook-modified uppercase text was not observed (expected token={expected})"
            )
            return 1

        print("PASS: chat hook behavior observed (/shout -> uppercase)")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
