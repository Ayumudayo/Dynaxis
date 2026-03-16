from __future__ import annotations

import argparse
import socket
import struct
import time
from dataclasses import dataclass


MSG_LOGIN_REQ = 0x0010
MSG_LOGIN_RES = 0x0011
MSG_UDP_BIND_REQ = 0x0012
MSG_UDP_BIND_RES = 0x0013
MSG_FPS_INPUT = 0x0206
MSG_FPS_STATE_SNAPSHOT = 0x0207
MSG_FPS_STATE_DELTA = 0x0208


@dataclass
class UdpBindTicket:
    code: int
    session_id: str
    nonce: int
    expires_unix_ms: int
    token: str
    message: str


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify TCP snapshot + direct UDP FPS delta transport shape")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36100)
    parser.add_argument("--udp-port", type=int, default=7000)
    parser.add_argument("--timeout-sec", type=float, default=5.0)
    args = parser.parse_args()

    seq = 1
    user = f"verify_fps_{int(time.time() * 1000)}"

    tcp_sock = socket.create_connection((args.host, args.port), timeout=args.timeout_sec)
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("0.0.0.0", 0))
    udp_sock.settimeout(args.timeout_sec)

    try:
        send_frame(tcp_sock, MSG_LOGIN_REQ, seq, lp_utf8(user) + lp_utf8(""))
        seq += 1

        login_ok = False
        ticket: UdpBindTicket | None = None
        deadline = time.monotonic() + args.timeout_sec
        while time.monotonic() < deadline:
            msg_id, frame_seq, payload = recv_frame(tcp_sock, args.timeout_sec)
            if msg_id == MSG_LOGIN_RES:
                login_ok = True
            elif msg_id == MSG_UDP_BIND_RES:
                ticket = parse_udp_bind_ticket(payload)
            if login_ok and ticket is not None:
                break

        if not login_ok or ticket is None:
            print("FAIL: login or UDP bind ticket was not received")
            return 1
        if ticket.code != 0:
            print(f"FAIL: UDP bind ticket rejected: code={ticket.code} message={ticket.message}")
            return 1

        bind_payload = (
            lp_utf8(ticket.session_id)
            + struct.pack(">Q", ticket.nonce)
            + struct.pack(">Q", ticket.expires_unix_ms)
            + lp_utf8(ticket.token)
        )
        udp_frame = struct.pack(">HHHII", len(bind_payload), MSG_UDP_BIND_REQ, 0, seq, int(time.time() * 1000) & 0xFFFFFFFF)
        udp_sock.sendto(udp_frame + bind_payload, (args.host, args.udp_port))
        expected_bind_seq = seq
        seq += 1

        while True:
            data, _sender = udp_sock.recvfrom(2048)
            if len(data) < 14:
                continue
            length, msg_id, _flags, bind_seq, _ts = struct.unpack(">HHHII", data[:14])
            if msg_id == MSG_UDP_BIND_RES and bind_seq == expected_bind_seq:
                if length != len(data) - 14:
                    print("FAIL: invalid UDP bind response frame length")
                    return 1
                bind_ticket = parse_udp_bind_ticket(data[14:])
                if bind_ticket.code != 0:
                    print(f"FAIL: UDP bind response rejected: code={bind_ticket.code} message={bind_ticket.message}")
                    return 1
                break

        fps_payload_1 = encode_fps_input(1, 100, 0, 9000)
        fps_frame_1 = struct.pack(">HHHII", len(fps_payload_1), MSG_FPS_INPUT, 0, seq, int(time.time() * 1000) & 0xFFFFFFFF)
        udp_sock.sendto(fps_frame_1 + fps_payload_1, (args.host, args.udp_port))
        seq += 1

        snapshot_payload = b""
        deadline = time.monotonic() + args.timeout_sec
        while time.monotonic() < deadline:
            msg_id, _frame_seq, payload = recv_frame(tcp_sock, args.timeout_sec)
            if msg_id == MSG_FPS_STATE_SNAPSHOT:
                snapshot_payload = payload
                break
        if not snapshot_payload:
            print("FAIL: FPS snapshot was not received over TCP")
            return 1

        snapshot = parse_fps_update(snapshot_payload)
        if int(snapshot["server_tick"]) == 0 or int(snapshot["self_actor_id"]) == 0:
            print(f"FAIL: Invalid snapshot metadata: {snapshot}")
            return 1
        if len(snapshot["actors"]) == 0:
            print(f"FAIL: Snapshot actor list is empty: {snapshot}")
            return 1

        fps_payload_2 = encode_fps_input(2, 100, 25, 18000)
        fps_frame_2 = struct.pack(">HHHII", len(fps_payload_2), MSG_FPS_INPUT, 0, seq, int(time.time() * 1000) & 0xFFFFFFFF)
        udp_sock.sendto(fps_frame_2 + fps_payload_2, (args.host, args.udp_port))

        delta_payload = b""
        deadline = time.monotonic() + args.timeout_sec
        while time.monotonic() < deadline:
            data, _sender = udp_sock.recvfrom(2048)
            length, msg_id, _flags, _seq, _ts = struct.unpack(">HHHII", data[:14])
            if msg_id != MSG_FPS_STATE_DELTA:
                continue
            if length != len(data) - 14:
                print("FAIL: invalid direct UDP delta frame length")
                return 1
            delta_payload = data[14:]
            break
        if not delta_payload:
            print("FAIL: FPS delta was not received over direct UDP")
            return 1

        delta = parse_fps_update(delta_payload)
        if int(delta["server_tick"]) == 0 or int(delta["self_actor_id"]) == 0:
            print(f"FAIL: Invalid delta metadata: {delta}")
            return 1
        if len(delta["actors"]) == 0:
            print(f"FAIL: Delta actor list is empty: {delta}")
            return 1

        print(f"PASS: TCP snapshot + direct UDP delta verified snapshot={snapshot} delta={delta}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: {exc}")
        return 1
    finally:
        tcp_sock.close()
        udp_sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
