"""Local development stack: shared authority, committed IDs and rollback visibility."""
from __future__ import annotations

import concurrent.futures
import socket
import subprocess
import time

from session_continuity_common import ChatClient, MSG_CHAT_SEND, MSG_JOIN_ROOM, lp_utf8, read_varint


def fields(payload):
    result = {}
    offset = 0
    while offset < len(payload):
        tag, offset = read_varint(payload, offset)
        number, wire = tag >> 3, tag & 7
        if wire == 0:
            value, offset = read_varint(payload, offset)
        elif wire == 2:
            length, offset = read_varint(payload, offset)
            value = payload[offset:offset + length]
            offset += length
        else:
            raise AssertionError(f"unexpected protobuf wire {wire}")
        result.setdefault(number, []).append(value)
    return result


def frame_until(client, opcode, predicate=lambda _: True, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        actual, _, _, _, body = client.recv_frame(deadline - time.monotonic())
        if actual == opcode and predicate(body):
            return body
    raise AssertionError(f"opcode {opcode} not received")


def sql(statement):
    result = subprocess.run(["docker", "exec", "-i", "dynaxis-stack-postgres-1", "sh", "-lc",
        'PGPASSWORD="$POSTGRES_PASSWORD" psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -tA'],
        input=statement, text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, "local database fault setup/query failed: " + result.stderr
    return result.stdout.strip()


def main():
    suffix = str(time.time_ns())
    clients = []

    def login(port, name):
        client = ChatClient(port=port)
        clients.append(client)
        client.connect()
        client.login(name, "")
        return client

    def send(client, room, text):
        client.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(text))

    try:
        owner_name, other_name = "owner-" + suffix, "member-" + suffix
        owner, other = login(30001, owner_name), login(30002, other_name)
        duplicate = ChatClient(port=30002)
        clients.append(duplicate)
        duplicate.connect()
        duplicate.send_frame(0x10, lp_utf8(owner_name) + lp_utf8(""))
        code, _ = duplicate.wait_for_error(5)
        assert code == 0x100, "cross-node duplicate principal admitted"
        print("PASS: principal reservation rejects a second backend login")

        room = "authority-" + suffix
        owner.join_room(room, owner_name)
        for spelling in (room, room.upper()):
            other.send_frame(MSG_JOIN_ROOM, lp_utf8(spelling) + lp_utf8(""))
            code, _ = other.wait_for_error(5)
            assert code == 0x102, "uninvited remote/case-alias join admitted"
        owner.invite(room, other_name)
        other.join_room(room, other_name)
        send(other, room, "/invite uninvolved")
        frame_until(other, 0x101, lambda body: b"owner/admin only" in body)
        print("PASS: shared invite works across nodes; remote membership grants no ownership")

        message = "committed-" + suffix
        send(owner, room, message)
        event = fields(frame_until(other, 0x101, lambda body: message.encode() in body))
        message_id = event[6][0]
        room_id = event[7][0]
        assert message_id > 0 and room_id
        other.send_frame(0x204, lp_utf8(room))
        snapshot = fields(frame_until(other, 0x200, lambda body: fields(body).get(7, [0])[0] >= message_id))
        assert snapshot[7][0] >= message_id and snapshot[8][0] == 1
        assert any(fields(item).get(1) == [message_id] for item in snapshot.get(4, []))
        print("PASS: live delivery and DB snapshot carry the same committed ID and watermark")

        failed = "rollback-" + suffix
        trigger, function = "audit_commit_" + suffix, "audit_commit_fn_" + suffix
        try:
            sql(f"""create function {function}() returns trigger language plpgsql as $$
begin raise exception 'injected deferred chat commit failure'; end; $$;
create constraint trigger {trigger} after insert on messages deferrable initially deferred
for each row when (new.content = '{failed}') execute function {function}();""")
            send(owner, room, failed)
            code, reason = owner.wait_for_error(5)
            assert code == 8 and "not confirmed" in reason
            assert sql(f"select count(*) from messages where content='{failed}';") == "0"
            deadline = time.monotonic() + .4
            while time.monotonic() < deadline:
                try:
                    _, _, _, _, payload = other.recv_frame(deadline - time.monotonic())
                except socket.timeout:
                    break
                assert failed.encode() not in payload, "rolled-back message escaped to remote recipient"
            print("PASS: deferred COMMIT failure rolls back and never reaches the remote recipient")
        finally:
            sql(f"drop trigger if exists {trigger} on messages; drop function if exists {function}();")

        send(owner, room, "/kick " + other_name)
        frame_until(owner, 0x101, lambda body: b"kick applied" in body)
        send(other, room, "must-be-revoked")
        code, _ = other.wait_for_error(5)
        assert code in (0x102, 0x106), "remote kick did not revoke room access"
        print("PASS: remote kick is enforced by shared state on the next command")

        contenders = [login(30001, "race-a-" + suffix), login(30002, "race-b-" + suffix)]
        race_room = "race-" + suffix
        def enter(client):
            client.send_frame(MSG_JOIN_ROOM, lp_utf8(race_room) + lp_utf8(""))
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                opcode, _, _, _, payload = client.recv_frame(deadline - time.monotonic())
                if opcode == 4:
                    assert int.from_bytes(payload[:2], "big") == 0x102
                    return "denied"
                if opcode == 0x101 and race_room.encode() in payload:
                    return "created"
            raise AssertionError("room creation response missing")
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            outcomes = list(executor.map(enter, contenders))
        assert sorted(outcomes) == ["created", "denied"], outcomes
        print("PASS: concurrent cross-node room creation has exactly one owner")
    finally:
        for client in clients:
            client.close()


if __name__ == "__main__":
    main()
