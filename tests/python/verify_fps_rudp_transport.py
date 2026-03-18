from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from session_continuity_common import read_metric_sum
from session_continuity_common import read_metric_sum_labeled
from stack_topology import GENERATED_COMPOSE_PATH
from stack_topology import ensure_stack_topology_artifacts


MSG_LOGIN_REQ = 0x0010
MSG_LOGIN_RES = 0x0011
MSG_UDP_BIND_REQ = 0x0012
MSG_UDP_BIND_RES = 0x0013
MSG_FPS_INPUT = 0x0206
MSG_FPS_STATE_SNAPSHOT = 0x0207
MSG_FPS_STATE_DELTA = 0x0208

RUDP_MAGIC = 0x5255
RUDP_HEADER_BYTES = 34
RUDP_PACKET_HELLO = 1
RUDP_PACKET_HELLO_ACK = 2
RUDP_PACKET_DATA = 3

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPOSE_PROJECT_DIR = REPO_ROOT / "docker" / "stack"
COMPOSE_FILE = COMPOSE_PROJECT_DIR / "docker-compose.yml"
DEFAULT_COMPOSE_ENV_FILE = COMPOSE_PROJECT_DIR / ".env.rudp-attach.example"
COMPOSE_PROJECT_NAME = "dynaxis-stack"


@dataclass
class UdpBindTicket:
    code: int
    session_id: str
    nonce: int
    expires_unix_ms: int
    token: str
    message: str


@dataclass
class TransportSession:
    host: str
    port: int
    udp_port: int
    timeout_sec: float
    scenario: str
    tcp_sock: socket.socket
    udp_sock: socket.socket
    seq: int = 1
    rudp_connection_id: int = 0
    rudp_packet_number: int = 1

    def close(self) -> None:
        self.tcp_sock.close()
        self.udp_sock.close()


def resolve_repo_path(raw: str | Path) -> Path:
    candidate = Path(raw)
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


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


def send_frame(sock: socket.socket, msg_id: int, seq: int, payload: bytes = b"") -> None:
    ts_ms = int(time.time() * 1000) & 0xFFFFFFFF
    header = struct.pack(">HHHII", len(payload), msg_id, 0, seq, ts_ms)
    sock.sendall(header + payload)


def build_inner_frame(msg_id: int, seq: int, payload: bytes = b"") -> bytes:
    ts_ms = int(time.time() * 1000) & 0xFFFFFFFF
    header = struct.pack(">HHHII", len(payload), msg_id, 0, seq, ts_ms)
    return header + payload


def recv_frame(sock: socket.socket, timeout_sec: float) -> tuple[int, int, bytes]:
    sock.settimeout(timeout_sec)
    header = read_exact(sock, 14)
    length, msg_id, _flags, seq, _ts = struct.unpack(">HHHII", header)
    payload = read_exact(sock, length) if length else b""
    return msg_id, seq, payload


def read_lp_utf8(payload: memoryview, offset: int) -> tuple[str, int]:
    length = struct.unpack_from(">H", payload, offset)[0]
    offset += 2
    value = bytes(payload[offset : offset + length]).decode("utf-8")
    return value, offset + length


def parse_udp_bind_ticket(payload: bytes) -> UdpBindTicket:
    view = memoryview(payload)
    code = struct.unpack_from(">H", view, 0)[0]
    offset = 2
    session_id, offset = read_lp_utf8(view, offset)
    nonce = struct.unpack_from(">Q", view, offset)[0]
    offset += 8
    expires_unix_ms = struct.unpack_from(">Q", view, offset)[0]
    offset += 8
    token, offset = read_lp_utf8(view, offset)
    message, _ = read_lp_utf8(view, offset)
    return UdpBindTicket(code, session_id, nonce, expires_unix_ms, token, message)


def zigzag32(value: int) -> int:
    return ((value << 1) ^ (value >> 31)) & 0xFFFFFFFF


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        chunk = value & 0x7F
        value >>= 7
        if value:
            out.append(chunk | 0x80)
        else:
            out.append(chunk)
            return bytes(out)


def encode_fps_input(input_seq: int, move_x_mm: int, move_y_mm: int, yaw_mdeg: int) -> bytes:
    parts = [
        bytes([0x08]) + encode_varint(input_seq),
        bytes([0x10]) + encode_varint(zigzag32(move_x_mm)),
        bytes([0x18]) + encode_varint(zigzag32(move_y_mm)),
        bytes([0x20]) + encode_varint(zigzag32(yaw_mdeg)),
    ]
    return b"".join(parts)


def decode_varint(payload: bytes, offset: int) -> tuple[int, int]:
    shift = 0
    value = 0
    while True:
        byte = payload[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, offset
        shift += 7


def skip_field(payload: bytes, offset: int, wire_type: int) -> int:
    if wire_type == 0:
        _, offset = decode_varint(payload, offset)
        return offset
    if wire_type == 2:
        length, offset = decode_varint(payload, offset)
        return offset + length
    raise ValueError(f"unsupported wire type: {wire_type}")


def parse_fps_actor(payload: bytes) -> dict[str, int]:
    offset = 0
    out: dict[str, int] = {}
    while offset < len(payload):
        key, offset = decode_varint(payload, offset)
        field = key >> 3
        wire_type = key & 0x07
        if wire_type != 0:
            offset = skip_field(payload, offset, wire_type)
            continue
        value, offset = decode_varint(payload, offset)
        if field == 1:
            out["actor_id"] = value
        elif field == 5:
            out["last_applied_input_seq"] = value
        elif field == 6:
            out["server_tick"] = value
    return out


def parse_fps_update(payload: bytes) -> dict[str, object]:
    offset = 0
    server_tick = 0
    self_actor_id = 0
    actors: list[dict[str, int]] = []
    removed_actor_ids: list[int] = []

    while offset < len(payload):
        key, offset = decode_varint(payload, offset)
        field = key >> 3
        wire_type = key & 0x07
        if field == 1 and wire_type == 0:
            server_tick, offset = decode_varint(payload, offset)
            continue
        if field == 2 and wire_type == 0:
            self_actor_id, offset = decode_varint(payload, offset)
            continue
        if field == 3 and wire_type == 2:
            length, offset = decode_varint(payload, offset)
            actors.append(parse_fps_actor(payload[offset : offset + length]))
            offset += length
            continue
        if field == 4 and wire_type == 0:
            value, offset = decode_varint(payload, offset)
            removed_actor_ids.append(value)
            continue
        offset = skip_field(payload, offset, wire_type)

    return {
        "server_tick": server_tick,
        "self_actor_id": self_actor_id,
        "actors": actors,
        "removed_actor_ids": removed_actor_ids,
    }


def encode_rudp_packet(packet_type: int,
                       connection_id: int,
                       packet_number: int,
                       payload: bytes = b"",
                       *,
                       ack_largest: int = 0,
                       ack_mask: int = 0,
                       ack_delay_ms: int = 0,
                       channel: int = 0,
                       flags: int = 0) -> bytes:
    timestamp_ms = int(time.time() * 1000) & 0xFFFFFFFF
    _ = struct.pack(
        ">HBBIIIQHBBH",
        RUDP_MAGIC,
        1,
        packet_type,
        connection_id,
        packet_number,
        ack_largest,
        ack_mask,
        ack_delay_ms,
        channel,
        flags,
        len(payload),
    )
    return (
        struct.pack(">H", RUDP_MAGIC)
        + bytes([1, packet_type])
        + struct.pack(">I", connection_id)
        + struct.pack(">I", packet_number)
        + struct.pack(">I", ack_largest)
        + struct.pack(">Q", ack_mask)
        + struct.pack(">H", ack_delay_ms)
        + bytes([channel, flags])
        + struct.pack(">I", timestamp_ms)
        + struct.pack(">H", len(payload))
        + payload
    )


def decode_rudp_packet(data: bytes) -> dict[str, object] | None:
    if len(data) < RUDP_HEADER_BYTES:
        return None
    magic = struct.unpack_from(">H", data, 0)[0]
    if magic != RUDP_MAGIC:
        return None
    version = data[2]
    packet_type = data[3]
    connection_id = struct.unpack_from(">I", data, 4)[0]
    packet_number = struct.unpack_from(">I", data, 8)[0]
    ack_largest = struct.unpack_from(">I", data, 12)[0]
    ack_mask = struct.unpack_from(">Q", data, 16)[0]
    ack_delay_ms = struct.unpack_from(">H", data, 24)[0]
    channel = data[26]
    flags = data[27]
    timestamp_ms = struct.unpack_from(">I", data, 28)[0]
    payload_length = struct.unpack_from(">H", data, 32)[0]
    if len(data) != RUDP_HEADER_BYTES + payload_length:
        return None
    return {
        "version": version,
        "type": packet_type,
        "connection_id": connection_id,
        "packet_number": packet_number,
        "ack_largest": ack_largest,
        "ack_mask": ack_mask,
        "ack_delay_ms": ack_delay_ms,
        "channel": channel,
        "flags": flags,
        "timestamp_ms": timestamp_ms,
        "payload": data[RUDP_HEADER_BYTES:],
    }


def parse_inner_frame(frame: bytes) -> tuple[int, int, bytes]:
    if len(frame) < 14:
        raise ValueError("inner frame shorter than protocol header")
    length, msg_id, _flags, seq, _ts = struct.unpack(">HHHII", frame[:14])
    if len(frame) != 14 + length:
        raise ValueError("inner frame length mismatch")
    return msg_id, seq, frame[14:]


def compose_base_args(env_file: Path) -> list[str]:
    ensure_stack_topology_artifacts()
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
    if env_file.exists():
        args.extend(["--env-file", str(env_file)])
    return args


def run_compose(env_file: Path, *args: str) -> str:
    command = compose_base_args(env_file) + list(args)
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        encoding="utf-8",
        errors="replace",
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout.strip()


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


def wait_tcp_endpoint(host: str, port: int, timeout_sec: float = 60.0) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return
        except OSError:
            time.sleep(0.5)
    raise TimeoutError(f"tcp endpoint timeout on {host}:{port}")


def read_gateway_metric(metric_name: str, labels: dict[str, str] | None = None) -> float:
    if labels is None:
        return read_metric_sum(metric_name)
    return read_metric_sum_labeled(metric_name, labels)


def opcode_name(msg_id: int) -> str:
    if msg_id == MSG_FPS_STATE_SNAPSHOT:
        return "MSG_FPS_STATE_SNAPSHOT"
    if msg_id == MSG_FPS_STATE_DELTA:
        return "MSG_FPS_STATE_DELTA"
    return hex(msg_id)


def assert_valid_update(name: str, update: dict[str, object], *, require_actors: bool) -> None:
    if int(update["server_tick"]) == 0 or int(update["self_actor_id"]) == 0:
        raise AssertionError(f"invalid {name} metadata: {update}")
    if require_actors and len(update["actors"]) == 0:
        raise AssertionError(f"{name} actor list is empty: {update}")


def open_transport_session(host: str,
                           port: int,
                           udp_port: int,
                           timeout_sec: float,
                           scenario: str) -> TransportSession:
    tcp_sock = socket.create_connection((host, port), timeout=timeout_sec)
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("0.0.0.0", 0))
    udp_sock.settimeout(timeout_sec)
    return TransportSession(
        host=host,
        port=port,
        udp_port=udp_port,
        timeout_sec=timeout_sec,
        scenario=scenario,
        tcp_sock=tcp_sock,
        udp_sock=udp_sock,
    )


def login_and_wait_for_ticket(session: TransportSession) -> UdpBindTicket:
    user = f"verify_rudp_{session.scenario}_{int(time.time() * 1000)}"
    send_frame(session.tcp_sock, MSG_LOGIN_REQ, session.seq, lp_utf8(user) + lp_utf8(""))
    session.seq += 1

    login_ok = False
    ticket: UdpBindTicket | None = None
    deadline = time.monotonic() + session.timeout_sec
    while time.monotonic() < deadline:
        msg_id, _frame_seq, payload = recv_frame(session.tcp_sock, session.timeout_sec)
        if msg_id == MSG_LOGIN_RES:
            login_ok = True
        elif msg_id == MSG_UDP_BIND_RES:
            ticket = parse_udp_bind_ticket(payload)
        if login_ok and ticket is not None:
            break

    if not login_ok or ticket is None:
        raise AssertionError("login or UDP bind ticket was not received")
    if ticket.code != 0:
        raise AssertionError(f"UDP bind ticket rejected: code={ticket.code} message={ticket.message}")
    return ticket


def bind_udp(session: TransportSession, ticket: UdpBindTicket) -> None:
    bind_payload = (
        lp_utf8(ticket.session_id)
        + struct.pack(">Q", ticket.nonce)
        + struct.pack(">Q", ticket.expires_unix_ms)
        + lp_utf8(ticket.token)
    )
    udp_frame = struct.pack(
        ">HHHII",
        len(bind_payload),
        MSG_UDP_BIND_REQ,
        0,
        session.seq,
        int(time.time() * 1000) & 0xFFFFFFFF,
    )
    session.udp_sock.sendto(udp_frame + bind_payload, (session.host, session.udp_port))
    expected_bind_seq = session.seq
    session.seq += 1

    deadline = time.monotonic() + session.timeout_sec
    while time.monotonic() < deadline:
        data, _sender = session.udp_sock.recvfrom(4096)
        if len(data) < 14:
            continue
        length, msg_id, _flags, bind_seq, _ts = struct.unpack(">HHHII", data[:14])
        if msg_id != MSG_UDP_BIND_RES or bind_seq != expected_bind_seq:
            continue
        if length != len(data) - 14:
            raise AssertionError("invalid UDP bind response frame length")
        bind_ticket = parse_udp_bind_ticket(data[14:])
        if bind_ticket.code != 0:
            raise AssertionError(
                f"UDP bind response rejected: code={bind_ticket.code} message={bind_ticket.message}"
            )
        return

    raise AssertionError("UDP bind response was not received")


def send_udp_fps_input(session: TransportSession,
                       input_seq: int,
                       move_x_mm: int,
                       move_y_mm: int,
                       yaw_mdeg: int) -> None:
    send_udp_fps_input_with_frame_seq(
        session,
        session.seq,
        input_seq,
        move_x_mm,
        move_y_mm,
        yaw_mdeg,
    )


def send_udp_fps_input_with_frame_seq(session: TransportSession,
                                      frame_seq: int,
                                      input_seq: int,
                                      move_x_mm: int,
                                      move_y_mm: int,
                                      yaw_mdeg: int) -> None:
    payload = encode_fps_input(input_seq, move_x_mm, move_y_mm, yaw_mdeg)
    frame = struct.pack(
        ">HHHII",
        len(payload),
        MSG_FPS_INPUT,
        0,
        frame_seq,
        int(time.time() * 1000) & 0xFFFFFFFF,
    )
    session.udp_sock.sendto(frame + payload, (session.host, session.udp_port))
    if frame_seq >= session.seq:
        session.seq = frame_seq + 1


def send_rudp_hello(session: TransportSession) -> None:
    session.rudp_connection_id = int(time.time() * 1000) & 0xFFFFFFFF
    session.rudp_packet_number = 1
    hello = encode_rudp_packet(RUDP_PACKET_HELLO, session.rudp_connection_id, 0)
    session.udp_sock.sendto(hello, (session.host, session.udp_port))


def wait_for_rudp_hello_ack(session: TransportSession) -> None:
    deadline = time.monotonic() + session.timeout_sec
    while time.monotonic() < deadline:
        data, _sender = session.udp_sock.recvfrom(4096)
        decoded = decode_rudp_packet(data)
        if not decoded:
            continue
        if decoded["type"] == RUDP_PACKET_HELLO_ACK and decoded["connection_id"] == session.rudp_connection_id:
            return
    raise AssertionError("RUDP HELLO_ACK was not received")


def assert_no_rudp_hello_ack(session: TransportSession, probe_timeout_sec: float) -> None:
    deadline = time.monotonic() + probe_timeout_sec
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        session.udp_sock.settimeout(max(0.1, min(remaining, 0.5)))
        try:
            data, _sender = session.udp_sock.recvfrom(4096)
        except socket.timeout:
            continue
        decoded = decode_rudp_packet(data)
        if not decoded:
            continue
        if decoded["type"] == RUDP_PACKET_HELLO_ACK and decoded["connection_id"] == session.rudp_connection_id:
            raise AssertionError("RUDP HELLO_ACK was received unexpectedly")
    session.udp_sock.settimeout(session.timeout_sec)


def send_rudp_fps_input(session: TransportSession,
                        input_seq: int,
                        move_x_mm: int,
                        move_y_mm: int,
                        yaw_mdeg: int) -> None:
    payload = encode_fps_input(input_seq, move_x_mm, move_y_mm, yaw_mdeg)
    datagram = encode_rudp_packet(
        RUDP_PACKET_DATA,
        session.rudp_connection_id,
        session.rudp_packet_number,
        build_inner_frame(MSG_FPS_INPUT, session.seq, payload),
        channel=1,
    )
    session.udp_sock.sendto(datagram, (session.host, session.udp_port))
    session.seq += 1
    session.rudp_packet_number += 1


def send_invalid_rudp_payload(session: TransportSession) -> None:
    datagram = encode_rudp_packet(
        RUDP_PACKET_DATA,
        session.rudp_connection_id,
        session.rudp_packet_number,
        b"\x00\x01",
        channel=1,
    )
    session.udp_sock.sendto(datagram, (session.host, session.udp_port))
    session.rudp_packet_number += 1


def wait_for_tcp_update(session: TransportSession, *expected_msg_ids: int) -> tuple[int, dict[str, object]]:
    deadline = time.monotonic() + session.timeout_sec
    while time.monotonic() < deadline:
        msg_id, _frame_seq, payload = recv_frame(session.tcp_sock, session.timeout_sec)
        if msg_id not in expected_msg_ids:
            continue
        update = parse_fps_update(payload)
        return msg_id, update
    raise AssertionError(f"expected TCP update was not received: {expected_msg_ids}")


def wait_for_direct_udp_delta(session: TransportSession) -> dict[str, object]:
    deadline = time.monotonic() + session.timeout_sec
    while time.monotonic() < deadline:
        data, _sender = session.udp_sock.recvfrom(4096)
        if len(data) < 14:
            continue
        length, msg_id, _flags, _seq, _ts = struct.unpack(">HHHII", data[:14])
        if msg_id != MSG_FPS_STATE_DELTA:
            continue
        if length != len(data) - 14:
            raise AssertionError("invalid direct UDP delta frame length")
        update = parse_fps_update(data[14:])
        assert_valid_update("direct UDP delta", update, require_actors=True)
        return update
    raise AssertionError("FPS delta was not received over direct UDP")


def wait_for_udp_error_response(session: TransportSession,
                                expected_frame_seq: int,
                                expected_message_substring: str) -> UdpBindTicket:
    deadline = time.monotonic() + session.timeout_sec
    while time.monotonic() < deadline:
        data, _sender = session.udp_sock.recvfrom(4096)
        if len(data) < 14:
            continue
        length, msg_id, _flags, frame_seq, _ts = struct.unpack(">HHHII", data[:14])
        if msg_id != MSG_UDP_BIND_RES or frame_seq != expected_frame_seq:
            continue
        if length != len(data) - 14:
            raise AssertionError("invalid UDP error response frame length")
        response = parse_udp_bind_ticket(data[14:])
        if response.code == 0:
            raise AssertionError(f"expected UDP error response, got success payload={response}")
        if expected_message_substring not in response.message:
            raise AssertionError(
                f"unexpected UDP error response message={response.message!r} expected~={expected_message_substring!r}"
            )
        return response
    raise AssertionError(f"expected UDP error response for frame_seq={expected_frame_seq} was not received")


def wait_for_direct_rudp_delta(session: TransportSession) -> dict[str, object]:
    deadline = time.monotonic() + session.timeout_sec
    while time.monotonic() < deadline:
        data, _sender = session.udp_sock.recvfrom(4096)
        decoded = decode_rudp_packet(data)
        if not decoded or decoded["type"] != RUDP_PACKET_DATA:
            continue
        try:
            msg_id, _inner_seq, payload = parse_inner_frame(decoded["payload"])
        except ValueError:
            continue
        if msg_id != MSG_FPS_STATE_DELTA:
            continue
        update = parse_fps_update(payload)
        assert_valid_update("direct RUDP delta", update, require_actors=True)
        return update
    raise AssertionError("FPS delta was not received over direct RUDP")


def assert_no_direct_state_update(session: TransportSession, probe_timeout_sec: float) -> None:
    deadline = time.monotonic() + probe_timeout_sec
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        session.udp_sock.settimeout(max(0.1, min(remaining, 0.5)))
        try:
            data, _sender = session.udp_sock.recvfrom(4096)
        except socket.timeout:
            continue

        decoded = decode_rudp_packet(data)
        if decoded and decoded["type"] == RUDP_PACKET_DATA:
            try:
                msg_id, _inner_seq, _payload = parse_inner_frame(decoded["payload"])
            except ValueError:
                continue
            if msg_id in {MSG_FPS_STATE_SNAPSHOT, MSG_FPS_STATE_DELTA}:
                raise AssertionError("direct RUDP state update was received after fallback latched")
            continue

        if len(data) < 14:
            continue
        length, msg_id, _flags, _seq, _ts = struct.unpack(">HHHII", data[:14])
        if msg_id in {MSG_FPS_STATE_SNAPSHOT, MSG_FPS_STATE_DELTA} and length == len(data) - 14:
            raise AssertionError("direct UDP state update was received after fallback latched")

    session.udp_sock.settimeout(session.timeout_sec)


def run_attach(args: argparse.Namespace, scenario_label: str = "attach") -> None:
    session = open_transport_session(args.host, args.port, args.udp_port, args.timeout_sec, scenario_label)
    try:
        ticket = login_and_wait_for_ticket(session)
        bind_udp(session, ticket)
        send_rudp_hello(session)
        wait_for_rudp_hello_ack(session)

        send_rudp_fps_input(session, 1, 100, 0, 9000)
        msg_id, snapshot = wait_for_tcp_update(session, MSG_FPS_STATE_SNAPSHOT)
        if msg_id != MSG_FPS_STATE_SNAPSHOT:
            raise AssertionError(f"expected TCP snapshot, got {opcode_name(msg_id)}")
        assert_valid_update("TCP snapshot", snapshot, require_actors=True)

        send_rudp_fps_input(session, 2, 100, 25, 18000)
        delta = wait_for_direct_rudp_delta(session)

        print(f"PASS {scenario_label}: TCP snapshot + direct RUDP delta verified snapshot={snapshot} delta={delta}")
    finally:
        session.close()


def run_disabled_mode(args: argparse.Namespace, scenario_label: str) -> None:
    disabled_before = read_gateway_metric(
        "core_runtime_rudp_fallback_total",
        {"reason": "disabled"},
    )
    gateway_fallback_before = read_gateway_metric("gateway_rudp_fallback_total")

    session = open_transport_session(args.host, args.port, args.udp_port, args.timeout_sec, scenario_label)
    try:
        ticket = login_and_wait_for_ticket(session)
        bind_udp(session, ticket)

        send_rudp_hello(session)
        assert_no_rudp_hello_ack(session, args.no_ack_probe_timeout_sec)

        disabled_after = read_gateway_metric(
            "core_runtime_rudp_fallback_total",
            {"reason": "disabled"},
        )
        gateway_fallback_after = read_gateway_metric("gateway_rudp_fallback_total")
        if disabled_after <= disabled_before:
            raise AssertionError(
                "disabled fallback metric did not increase: "
                f"before={disabled_before} after={disabled_after}"
            )
        if gateway_fallback_after <= gateway_fallback_before:
            raise AssertionError(
                "gateway fallback metric did not increase: "
                f"before={gateway_fallback_before} after={gateway_fallback_after}"
            )

        send_udp_fps_input(session, 1, 100, 0, 9000)
        msg_id, snapshot = wait_for_tcp_update(session, MSG_FPS_STATE_SNAPSHOT)
        if msg_id != MSG_FPS_STATE_SNAPSHOT:
            raise AssertionError(f"expected TCP snapshot, got {opcode_name(msg_id)}")
        assert_valid_update("TCP snapshot", snapshot, require_actors=True)

        send_udp_fps_input(session, 2, 100, 25, 18000)
        delta = wait_for_direct_udp_delta(session)

        print(
            f"PASS {scenario_label}: disabled RUDP attach fell back cleanly "
            f"disabled_delta={disabled_after - disabled_before:.0f} "
            f"gateway_delta={gateway_fallback_after - gateway_fallback_before:.0f} "
            f"snapshot={snapshot} delta={delta}"
        )
    finally:
        session.close()


def run_protocol_fallback(args: argparse.Namespace) -> None:
    protocol_before = read_gateway_metric(
        "core_runtime_rudp_fallback_total",
        {"reason": "protocol_error"},
    )
    gateway_fallback_before = read_gateway_metric("gateway_rudp_fallback_total")
    tcp_fallback_before = read_gateway_metric("gateway_direct_state_delta_tcp_fallback_total")

    session = open_transport_session(args.host, args.port, args.udp_port, args.timeout_sec, "protocol_fallback")
    try:
        ticket = login_and_wait_for_ticket(session)
        bind_udp(session, ticket)
        send_rudp_hello(session)
        wait_for_rudp_hello_ack(session)

        send_rudp_fps_input(session, 1, 100, 0, 9000)
        msg_id, snapshot = wait_for_tcp_update(session, MSG_FPS_STATE_SNAPSHOT)
        if msg_id != MSG_FPS_STATE_SNAPSHOT:
            raise AssertionError(f"expected TCP snapshot, got {opcode_name(msg_id)}")
        assert_valid_update("TCP snapshot", snapshot, require_actors=True)

        send_invalid_rudp_payload(session)
        time.sleep(0.5)

        send_udp_fps_input(session, 2, 100, 25, 18000)
        tcp_msg_id, update = wait_for_tcp_update(session, MSG_FPS_STATE_SNAPSHOT, MSG_FPS_STATE_DELTA)
        assert_valid_update("TCP fallback update", update, require_actors=True)
        assert_no_direct_state_update(session, args.no_direct_probe_timeout_sec)

        protocol_after = read_gateway_metric(
            "core_runtime_rudp_fallback_total",
            {"reason": "protocol_error"},
        )
        gateway_fallback_after = read_gateway_metric("gateway_rudp_fallback_total")
        tcp_fallback_after = read_gateway_metric("gateway_direct_state_delta_tcp_fallback_total")

        if protocol_after <= protocol_before:
            raise AssertionError(
                "protocol-error fallback metric did not increase: "
                f"before={protocol_before} after={protocol_after}"
            )
        if gateway_fallback_after <= gateway_fallback_before:
            raise AssertionError(
                "gateway fallback metric did not increase: "
                f"before={gateway_fallback_before} after={gateway_fallback_after}"
            )
        if tcp_fallback_after <= tcp_fallback_before:
            raise AssertionError(
                "direct delta TCP fallback metric did not increase: "
                f"before={tcp_fallback_before} after={tcp_fallback_after}"
            )

        print(
            "PASS protocol-fallback: "
            f"tcp_msg={opcode_name(tcp_msg_id)} "
            f"protocol_delta={protocol_after - protocol_before:.0f} "
            f"gateway_delta={gateway_fallback_after - gateway_fallback_before:.0f} "
            f"tcp_fallback_delta={tcp_fallback_after - tcp_fallback_before:.0f} "
            f"snapshot={snapshot} update={update}"
        )
    finally:
        session.close()


def run_udp_quality_impairment(args: argparse.Namespace) -> None:
    loss_before = read_gateway_metric("gateway_udp_loss_estimated_total")
    replay_before = read_gateway_metric("gateway_udp_replay_drop_total")
    reorder_before = read_gateway_metric("gateway_udp_reorder_drop_total")
    duplicate_before = read_gateway_metric("gateway_udp_duplicate_drop_total")
    bind_reject_before = read_gateway_metric("gateway_udp_bind_reject_total")
    jitter_before = read_gateway_metric("gateway_udp_jitter_ms_last")

    session = open_transport_session(args.host, args.port, args.udp_port, args.timeout_sec, "udp_quality_impairment")
    try:
        ticket = login_and_wait_for_ticket(session)
        bind_udp(session, ticket)

        send_udp_fps_input_with_frame_seq(session, 100, 1, 100, 0, 9000)
        msg_id, snapshot = wait_for_tcp_update(session, MSG_FPS_STATE_SNAPSHOT)
        if msg_id != MSG_FPS_STATE_SNAPSHOT:
            raise AssertionError(f"expected TCP snapshot, got {opcode_name(msg_id)}")
        assert_valid_update("TCP snapshot", snapshot, require_actors=True)

        time.sleep(args.udp_impairment_initial_gap_sec)
        send_udp_fps_input_with_frame_seq(session, 102, 2, 120, 25, 18000)
        delta_after_gap = wait_for_direct_udp_delta(session)

        time.sleep(args.udp_impairment_reorder_gap_sec)
        send_udp_fps_input_with_frame_seq(session, 101, 3, 140, 25, 24000)
        reorder_error = wait_for_udp_error_response(session, 101, "stale sequenced udp packet")

        send_udp_fps_input_with_frame_seq(session, 102, 4, 160, 25, 27000)
        duplicate_error = wait_for_udp_error_response(session, 102, "stale sequenced udp packet")

        time.sleep(args.udp_impairment_final_gap_sec)
        send_udp_fps_input_with_frame_seq(session, 103, 5, 180, 50, 32000)
        delta_after_impairment = wait_for_direct_udp_delta(session)

        loss_after = read_gateway_metric("gateway_udp_loss_estimated_total")
        replay_after = read_gateway_metric("gateway_udp_replay_drop_total")
        reorder_after = read_gateway_metric("gateway_udp_reorder_drop_total")
        duplicate_after = read_gateway_metric("gateway_udp_duplicate_drop_total")
        bind_reject_after = read_gateway_metric("gateway_udp_bind_reject_total")
        jitter_after = read_gateway_metric("gateway_udp_jitter_ms_last")

        if loss_after <= loss_before:
            raise AssertionError(
                f"loss estimate metric did not increase: before={loss_before} after={loss_after}"
            )
        if replay_after < replay_before + 2:
            raise AssertionError(
                f"replay drop metric did not reflect stale packets: before={replay_before} after={replay_after}"
            )
        if reorder_after <= reorder_before:
            raise AssertionError(
                f"reorder drop metric did not increase: before={reorder_before} after={reorder_after}"
            )
        if duplicate_after <= duplicate_before:
            raise AssertionError(
                f"duplicate drop metric did not increase: before={duplicate_before} after={duplicate_after}"
            )
        if bind_reject_after < bind_reject_before + 2:
            raise AssertionError(
                f"bind reject metric did not reflect stale packets: before={bind_reject_before} after={bind_reject_after}"
            )
        if jitter_after < args.min_expected_udp_jitter_ms:
            raise AssertionError(
                f"jitter gauge did not reach expected threshold: before={jitter_before} after={jitter_after} "
                f"threshold={args.min_expected_udp_jitter_ms}"
            )

        print(
            "PASS udp-quality-impairment: "
            f"loss_delta={loss_after - loss_before:.0f} "
            f"replay_delta={replay_after - replay_before:.0f} "
            f"reorder_delta={reorder_after - reorder_before:.0f} "
            f"duplicate_delta={duplicate_after - duplicate_before:.0f} "
            f"bind_reject_delta={bind_reject_after - bind_reject_before:.0f} "
            f"jitter_before={jitter_before:.0f} jitter_after={jitter_after:.0f} "
            f"reorder_error={reorder_error.message!r} duplicate_error={duplicate_error.message!r} "
            f"gap_delta={delta_after_gap} final_delta={delta_after_impairment}"
        )
    finally:
        session.close()


def run_restart(args: argparse.Namespace) -> None:
    run_attach(args, "restart_before")

    env_file = resolve_repo_path(args.compose_env_file)
    run_compose(env_file, "restart", "gateway-1")
    wait_ready(args.restart_ready_port, args.restart_timeout_sec)
    wait_tcp_endpoint(args.host, args.port, args.restart_timeout_sec)
    time.sleep(2.0)

    run_attach(args, "restart_after")
    print(
        "PASS restart: fresh direct RUDP attach succeeded before and after gateway-1 restart"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify direct UDP/RUDP FPS transport behavior across attach/fallback/restart scenarios"
    )
    parser.add_argument(
        "--scenario",
        choices=(
            "attach",
            "off",
            "rollout-fallback",
            "protocol-fallback",
            "udp-quality-impairment",
            "restart",
        ),
        default="attach",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36100)
    parser.add_argument("--udp-port", type=int, default=7000)
    parser.add_argument("--timeout-sec", type=float, default=5.0)
    parser.add_argument("--no-ack-probe-timeout-sec", type=float, default=1.5)
    parser.add_argument("--no-direct-probe-timeout-sec", type=float, default=1.5)
    parser.add_argument("--compose-env-file", default=str(DEFAULT_COMPOSE_ENV_FILE))
    parser.add_argument("--restart-ready-port", type=int, default=36001)
    parser.add_argument("--restart-timeout-sec", type=float, default=60.0)
    parser.add_argument("--udp-impairment-initial-gap-sec", type=float, default=0.015)
    parser.add_argument("--udp-impairment-reorder-gap-sec", type=float, default=0.010)
    parser.add_argument("--udp-impairment-final-gap-sec", type=float, default=0.080)
    parser.add_argument("--min-expected-udp-jitter-ms", type=float, default=20.0)
    args = parser.parse_args()

    try:
        if args.scenario == "attach":
            run_attach(args)
        elif args.scenario == "off":
            run_disabled_mode(args, "off")
        elif args.scenario == "rollout-fallback":
            run_disabled_mode(args, "rollout_fallback")
        elif args.scenario == "protocol-fallback":
            run_protocol_fallback(args)
        elif args.scenario == "udp-quality-impairment":
            run_udp_quality_impairment(args)
        elif args.scenario == "restart":
            run_restart(args)
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
