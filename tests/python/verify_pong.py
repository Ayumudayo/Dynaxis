from __future__ import annotations

import argparse
import socket
import struct
from typing import cast


def verify_pong(host: str, port: int, timeout_sec: float) -> int:
    print(f"Connecting to {host}:{port}...")

    try:
        with socket.create_connection((host, port), timeout=timeout_sec) as sock:
            msg_id = 0x0002  # MSG_PING
            length = 0
            flags = 0
            seq = 1
            timestamp = 0
            header = struct.pack(">HHHI", length, msg_id, flags, seq) + struct.pack(
                ">I", timestamp
            )

            print("Sending MSG_PING...")
            sock.sendall(header)

            sock.settimeout(timeout_sec)
            try:
                data = sock.recv(1024)
            except socket.timeout:
                print("FAIL: Timed out waiting for MSG_PONG response")
                return 1

            if not data:
                print("FAIL: Connection closed after MSG_PING")
                return 1

            print(f"Received {len(data)} bytes: {data.hex()}")
            if len(data) < 8:
                print("FAIL: Response frame is shorter than protocol header")
                return 1

            unpacked = cast(tuple[int, int, int, int], struct.unpack(">HHHH", data[:8]))
            response_msg_id = unpacked[1]
            print(f"MsgID: {response_msg_id:#06x}")
            if response_msg_id != 0x0003:  # MSG_PONG
                print(f"FAIL: Expected MSG_PONG (0x0003), got {response_msg_id:#06x}")
                return 1

            print("PASS: Protocol accepted MSG_PING and returned MSG_PONG")
            return 0
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: Unable to verify ping/pong protocol: {exc}")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify MSG_PING -> MSG_PONG protocol path"
    )
    _ = parser.add_argument("--host", default="127.0.0.1", help="gateway host")
    _ = parser.add_argument("--port", type=int, default=6000, help="gateway port")
    _ = parser.add_argument(
        "--timeout-sec",
        type=float,
        default=2.0,
        help="socket connect/read timeout",
    )
    args = parser.parse_args()

    host = cast(str, args.host)
    port = cast(int, args.port)
    timeout_sec = cast(float, args.timeout_sec)
    return verify_pong(host, port, timeout_sec)


if __name__ == "__main__":
    raise SystemExit(main())
