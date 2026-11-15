"""Signed-mode stack proof. Credentials and continuity tokens are never printed."""
from __future__ import annotations

import hashlib
import argparse
import hmac
import json
import os
from pathlib import Path
import sys
import secrets
import subprocess
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from issue_chat_token import issue_token
from session_continuity_common import ChatClient, MSG_CHAT_SEND, MSG_LOGIN_REQ, lp_utf8
from verify_whisper_cross_instance import resolve_backend_with_retry


def expired_token(token: str, secret: str) -> str:
    payload = json.loads(bytes.fromhex(token.split(".")[1]))
    payload["iat"] = int(time.time()) - 120
    payload["exp"] = int(time.time()) - 60
    data = "chat1." + json.dumps(payload, separators=(",", ":")).encode().hex()
    return data + "." + hmac.new(secret.encode(), data.encode(), hashlib.sha256).hexdigest()


def reject(token: str, port: int) -> None:
    client = ChatClient(port=port)
    try:
        client.connect()
        client.send_frame(MSG_LOGIN_REQ, lp_utf8("impersonated") + lp_utf8(token))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                opcode, _, _, _, _ = client.recv_frame(deadline - time.monotonic())
            except (ConnectionError, ConnectionResetError):
                if port == 6000:
                    return
                raise
            if opcode == 0x0011:
                raise AssertionError("invalid credential was admitted")
            if opcode == 0x0004:
                return
        raise AssertionError("credential rejection not observed")
    finally:
        client.close()


def main() -> int:
    secret = os.environ["CHAT_AUTH_SIGNING_SECRET"]
    suffix = str(time.time_ns())
    first_subject, second_subject = "signed-a-" + suffix, "signed-b-" + suffix
    first_token = issue_token(first_subject, "First display label", secret)
    second_token = issue_token(second_subject, "Second display label", secret)
    for credential in ("", first_token + "x", expired_token(first_token, secret),
                       issue_token(first_subject, "label", "x" * 32)):
        for port in (6000, 30001):
            reject(credential, port)
    print("PASS: gateway and backend reject empty, tampered, expired and wrong-key credentials")
    clients = []
    try:
        first = ChatClient()
        clients.append(first)
        first.connect()
        login = first.login("forged-name", first_token)
        assert login["effective_user"] == login["principal_id"] == first_subject
        assert login["display_name"] == "First display label" and login["auth_session_id"]
        assert login["logical_session_id"] and login["resume_token"]
        backend = resolve_backend_with_retry(first_subject)
        assert backend in {"server-1", "server-2"}
        second = ChatClient(port=30002 if backend == "server-1" else 30001)
        clients.append(second)
        second.connect()
        second.login("another-forged-name", second_token)
        room = "signed-room-" + suffix
        first.join_room(room, first_subject)
        first.invite(room, second_subject)
        second.join_room(room, second_subject)
        message = "signed-fanout-" + suffix
        first.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        deadline = time.monotonic() + 5
        while True:
            opcode, _, _, _, payload = second.recv_frame(deadline - time.monotonic())
            if opcode == 0x0101 and message.encode() in payload:
                break
        print("PASS: verified identity overrides claimed name; authenticated cross-node chat delivered")
        thief = ChatClient()
        clients.append(thief)
        thief.connect()
        thief.send_frame(MSG_LOGIN_REQ, lp_utf8(first_subject) + lp_utf8(second_token + "|resume:" + login["resume_token"]))
        _, reason = thief.wait_for_error(5)
        assert reason == "resume identity mismatch"
        thief.close()
        resumed_client = ChatClient()
        clients.append(resumed_client)
        resumed_client.connect()
        resumed = resumed_client.login("ignored", first_token + "|resume:" + login["resume_token"])
        assert resumed["principal_id"] == first_subject and resumed["resumed"]
        assert resumed["logical_session_id"] == login["logical_session_id"]
        try:
            first.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8("stale-connection-must-not-send"))
            code, _ = first.wait_for_error(5)
            assert code == 0x0101
        except (ConnectionError, ConnectionResetError, BrokenPipeError):
            pass
        first.close()
        resumed_client.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8("after-resume"))
        resumed_client.wait_for_self_chat(room, "after-resume", 5)
        print("PASS: live resume fences the prior connection, preserves identity and rejects a different principal")
        return 0
    finally:
        for client in clients:
            client.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manage-stack", action="store_true", help="표준 deploy 스크립트로 signed 모드를 검증하고 원래 환경을 복구한다")
    args = parser.parse_args()
    if not args.manage_stack:
        raise SystemExit(main())
    names = ("CHAT_AUTH_MODE", "CHAT_AUTH_SIGNING_SECRET", "SESSION_CONTINUITY_ENABLED")
    original = {name: os.environ.get(name) for name in names}
    command = ["pwsh", "-NoProfile", "-File", str(Path(__file__).resolve().parents[2] / "scripts/deploy_docker.ps1"),
               "-Action", "up", "-Detached", "-NoBase"]

    def deploy() -> None:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError("standard stack deployment failed")
        deadline = time.monotonic() + 90
        pending = {36001, 36002, 39091, 39092, 39093}
        while pending and time.monotonic() < deadline:
            for port in tuple(pending):
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{port}/readyz", timeout=2) as response:
                        if response.status == 200:
                            pending.remove(port)
                except OSError:
                    pass
            time.sleep(0.2)
        if pending:
            raise RuntimeError("stack readiness timeout")

    try:
        os.environ.update(CHAT_AUTH_MODE="signed", CHAT_AUTH_SIGNING_SECRET=secrets.token_hex(32), SESSION_CONTINUITY_ENABLED="1")
        deploy()
        for name, binary in (("server-1", "server_app"), ("gateway-1", "gateway_app")):
            result = subprocess.run(["docker", "exec", f"dynaxis-stack-{name}-1", "env", "-u", "CHAT_AUTH_SIGNING_SECRET",
                "CHAT_AUTH_MODE=signed", f"/app/{binary}"], capture_output=True, timeout=15)
            assert result.returncode != 0 and b"signing secret" in result.stderr + result.stdout
        print("PASS: signed-mode binaries refuse startup without a signing secret")
        raise SystemExit(main())
    finally:
        for name, value in original.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        deploy()
