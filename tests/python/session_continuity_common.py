import socket
import time
import urllib.request

HOST = "127.0.0.1"
PORT = 6000

MSG_LOGIN_REQ = 0x0010
MSG_LOGIN_RES = 0x0011
MSG_CHAT_SEND = 0x0100
MSG_CHAT_BROADCAST = 0x0101
MSG_JOIN_ROOM = 0x0102
MSG_ERR = 0x0004
FLAG_SELF = 0x0100
GATEWAY_METRICS_PORTS = (36001, 36002)


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


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    shift = 0
    value = 0
    while True:
        if offset >= len(data):
            raise ValueError("truncated varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, offset
        shift += 7
        if shift > 63:
            raise ValueError("varint too large")


def skip_wire_value(data: bytes, offset: int, wire_type: int) -> int:
    if wire_type == 0:
        _, offset = read_varint(data, offset)
        return offset
    if wire_type == 2:
        length, offset = read_varint(data, offset)
        return offset + length
    raise ValueError(f"unsupported wire type: {wire_type}")


def parse_login_res(payload: bytes) -> dict:
    result = {
        "effective_user": "",
        "session_id": 0,
        "message": "",
        "is_admin": False,
        "logical_session_id": "",
        "resume_token": "",
        "resume_expires_unix_ms": 0,
        "resumed": False,
        "world_id": "",
    }
    offset = 0
    while offset < len(payload):
        tag, offset = read_varint(payload, offset)
        field_number = tag >> 3
        wire_type = tag & 0x07

        if wire_type == 2 and field_number in {1, 3, 5, 6, 9}:
            length, offset = read_varint(payload, offset)
            raw = payload[offset:offset + length]
            offset += length
            value = raw.decode("utf-8")
            if field_number == 1:
                result["effective_user"] = value
            elif field_number == 3:
                result["message"] = value
            elif field_number == 5:
                result["logical_session_id"] = value
            elif field_number == 6:
                result["resume_token"] = value
            elif field_number == 9:
                result["world_id"] = value
            continue

        if wire_type == 0 and field_number in {2, 4, 7, 8}:
            value, offset = read_varint(payload, offset)
            if field_number == 2:
                result["session_id"] = value
            elif field_number == 4:
                result["is_admin"] = value != 0
            elif field_number == 7:
                result["resume_expires_unix_ms"] = value
            elif field_number == 8:
                result["resumed"] = value != 0
            continue

        offset = skip_wire_value(payload, offset, wire_type)

    return result


def read_metric_sum(metric_name: str, ports: tuple[int, ...] = GATEWAY_METRICS_PORTS) -> float:
    total = 0.0
    for port in ports:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=5.0) as response:
            text = response.read().decode("utf-8")
        for line in text.splitlines():
            if line.startswith(metric_name + " "):
                total += float(line.split(" ", 1)[1])
    return total


class ChatClient:
    def __init__(self, host: str = HOST, port: int = PORT) -> None:
        self.host = host
        self.port = port
        self.seq = 1
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    def connect(self) -> None:
        self.sock.connect((self.host, self.port))

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

    def wait_for_login(self, timeout_sec: float) -> dict:
        deadline = time.monotonic() + timeout_sec
        seen: list[int] = []
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise TimeoutError(f"login response timeout; seen={seen}")
            msg_id, _flags, _seq, _ts, payload = self.recv_frame(remain)
            seen.append(msg_id)
            if msg_id == MSG_LOGIN_RES:
                return parse_login_res(payload)

    def login(self, user: str, token: str, timeout_sec: float = 5.0) -> dict:
        self.send_frame(MSG_LOGIN_REQ, lp_utf8(user) + lp_utf8(token))
        return self.wait_for_login(timeout_sec)

    def wait_for_join_confirmation(self, room: str, user: str, timeout_sec: float) -> None:
        deadline = time.monotonic() + timeout_sec
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise TimeoutError("join confirmation timeout")
            msg_id, _flags, _seq, _ts, payload = self.recv_frame(remain)
            if msg_id != MSG_CHAT_BROADCAST:
                continue
            if room.encode("utf-8") in payload and user.encode("utf-8") in payload:
                return

    def join_room(self, room: str, user: str, timeout_sec: float = 5.0, password: str = "") -> None:
        self.send_frame(MSG_JOIN_ROOM, lp_utf8(room) + lp_utf8(password))
        self.wait_for_join_confirmation(room, user, timeout_sec)

    def wait_for_self_chat(self, room: str, text: str, timeout_sec: float) -> None:
        deadline = time.monotonic() + timeout_sec
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise TimeoutError("self chat timeout")
            msg_id, flags, _seq, _ts, payload = self.recv_frame(remain)
            if msg_id != MSG_CHAT_BROADCAST:
                continue
            if (flags & FLAG_SELF) == 0:
                continue
            if room.encode("utf-8") in payload and text.encode("utf-8") in payload:
                return

    def wait_for_error(self, timeout_sec: float) -> tuple[int, str]:
        deadline = time.monotonic() + timeout_sec
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise TimeoutError("error frame timeout")
            msg_id, _flags, _seq, _ts, payload = self.recv_frame(remain)
            if msg_id != MSG_ERR:
                continue
            if len(payload) < 4:
                raise ValueError("invalid error payload")
            code = int.from_bytes(payload[0:2], byteorder="big", signed=False)
            message_len = int.from_bytes(payload[2:4], byteorder="big", signed=False)
            message = payload[4:4 + message_len].decode("utf-8")
            return code, message
