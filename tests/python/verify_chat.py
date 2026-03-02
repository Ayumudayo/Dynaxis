import socket
import time

HOST = "127.0.0.1"
PORT = 6000  # HAProxy frontend

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
        self.seq: int = 1
        self.sock: socket.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

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

    def recv_frame(self, timeout_sec: float) -> tuple[int, int, int, int, bytes]:
        self.sock.settimeout(timeout_sec)
        header = read_exact(self.sock, 14)
        length = int.from_bytes(header[0:2], byteorder="big", signed=False)
        msg_id = int.from_bytes(header[2:4], byteorder="big", signed=False)
        flags = int.from_bytes(header[4:6], byteorder="big", signed=False)
        seq = int.from_bytes(header[6:10], byteorder="big", signed=False)
        ts = int.from_bytes(header[10:14], byteorder="big", signed=False)
        payload = read_exact(self.sock, length) if length else b""
        return msg_id, flags, seq, ts, payload

    def wait_for_login(self, timeout_sec: float) -> bool:
        deadline = time.monotonic() + timeout_sec
        seen: list[int] = []
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                print(f"Login response timeout; seen={seen}")
                return False
            try:
                msg_id, _flags, _seq, _ts, _payload = self.recv_frame(remain)
            except socket.timeout:
                print(f"Login response timeout; seen={seen}")
                return False
            seen.append(msg_id)
            if msg_id == MSG_LOGIN_RES:
                return True

    def wait_for_self_broadcast_with_text(self, text: str, timeout_sec: float) -> bool:
        target = text.encode("utf-8")
        deadline = time.monotonic() + timeout_sec
        seen: list[tuple[int, int]] = []
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                print(f"Chat broadcast timeout; seen={seen}")
                return False
            try:
                msg_id, flags, _seq, _ts, payload = self.recv_frame(remain)
            except socket.timeout:
                print(f"Chat broadcast timeout; seen={seen}")
                return False
            seen.append((msg_id, flags))
            if msg_id == MSG_CHAT_BROADCAST and (flags & FLAG_SELF) != 0 and target in payload:
                return True


def main() -> int:
    user = f"verify_chat_{int(time.time())}"
    message = f"hello_verification_msg_{int(time.time() * 1000)}"
    client = ChatClient()

    try:
        print(f"Connecting to {HOST}:{PORT}...")
        client.connect()

        print(f"Logging in as {user}...")
        client.send_frame(MSG_LOGIN_REQ, lp_utf8(user) + lp_utf8(""))
        if not client.wait_for_login(5.0):
            print("FAIL: login response was not received")
            return 1

        print("Joining lobby...")
        client.send_frame(MSG_JOIN_ROOM, lp_utf8("lobby") + lp_utf8(""))

        print(f"Sending chat message: {message}")
        client.send_frame(MSG_CHAT_SEND, lp_utf8("lobby") + lp_utf8(message))

        if not client.wait_for_self_broadcast_with_text(message, 5.0):
            print("FAIL: self chat broadcast with expected text was not observed")
            return 1

        print("PASS: chat message broadcast observed")
        return 0
    except Exception as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
