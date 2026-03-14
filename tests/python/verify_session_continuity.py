import time
from session_continuity_common import ChatClient
from session_continuity_common import HOST
from session_continuity_common import MSG_CHAT_SEND
from session_continuity_common import PORT
from session_continuity_common import lp_utf8
from session_continuity_common import read_metric_sum


def main() -> int:
    user = f"verify_resume_{int(time.time())}"
    room = f"resume_room_{int(time.time())}"
    message = f"resume_msg_{int(time.time() * 1000)}"

    first = ChatClient()
    second = ChatClient()

    try:
        print(f"Connecting to {HOST}:{PORT} for initial login...")
        first.connect()
        login = first.login(user, "")

        if login["effective_user"] != user:
            print(f"FAIL: effective user mismatch: {login}")
            return 1
        if not login["logical_session_id"] or not login["resume_token"] or login["resume_expires_unix_ms"] == 0:
            print(f"FAIL: continuity lease fields missing: {login}")
            return 1
        if not login["world_id"]:
            print(f"FAIL: world admission metadata missing: {login}")
            return 1
        if login["resumed"]:
            print(f"FAIL: initial login unexpectedly marked resumed: {login}")
            return 1

        logical_session_id = login["logical_session_id"]
        resume_token = login["resume_token"]
        world_id = login["world_id"]

        print(f"Joining continuity room {room}...")
        first.join_room(room, user)
        first.close()

        print("Reconnecting with resume token...")
        second.connect()
        resumed = second.login("ignored_resume_user", "resume:" + resume_token)

        if resumed["effective_user"] != user:
            print(f"FAIL: resumed effective user mismatch: {resumed}")
            return 1
        if resumed["logical_session_id"] != logical_session_id:
            print(f"FAIL: logical session id changed across resume: {resumed}")
            return 1
        if resumed["world_id"] != world_id:
            print(f"FAIL: world residency changed across resume: {resumed}")
            return 1
        if not resumed["resumed"]:
            print(f"FAIL: resumed login was not marked resumed: {resumed}")
            return 1

        print(f"Sending chat after resume to restored room {room}...")
        second.send_frame(MSG_CHAT_SEND, lp_utf8(room) + lp_utf8(message))
        second.wait_for_self_chat(room, message, 5.0)

        bind_total = read_metric_sum("gateway_resume_routing_bind_total")
        hit_total = read_metric_sum("gateway_resume_routing_hit_total")
        if bind_total < 1:
            print(f"FAIL: gateway resume routing bind counter did not increase: {bind_total}")
            return 1
        if hit_total < 1:
            print(f"FAIL: gateway resume routing hit counter did not increase: {hit_total}")
            return 1

        print("PASS: session continuity lease issued and resumed session preserved logical identity")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        first.close()
        second.close()


if __name__ == "__main__":
    raise SystemExit(main())
