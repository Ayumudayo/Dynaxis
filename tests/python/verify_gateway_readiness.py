"""Force an unroutable registry and verify admission readiness follows routing."""
import json
import subprocess
import time
import urllib.error
import urllib.request

from session_continuity_common import ChatClient


def redis(*args):
    return subprocess.check_output(["docker", "exec", "dynaxis-stack-redis-1", "redis-cli", "--raw", *args], text=True).strip()


def ready(port):
    try:
        return urllib.request.urlopen(f"http://127.0.0.1:{port}/readyz", timeout=2).status == 200
    except urllib.error.HTTPError as error:
        assert error.code == 503
        return False


def main():
    keys = redis("KEYS", "gateway/instances/*").splitlines()
    assert len(keys) >= 2, "expected two registered backends"
    originals = {key: redis("GET", key) for key in keys}
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            for key, raw in originals.items():
                record = json.loads(raw)
                record["ready"] = False
                redis("SETEX", key, "30", json.dumps(record, separators=(",", ":")))
            if not ready(36001) and not ready(36002):
                break
            time.sleep(.1)
        else:
            raise AssertionError("gateway reported ready without a routable backend")
        print("PASS: no routable backend lowers both gateway readiness endpoints")
    finally:
        for key, raw in originals.items():
            redis("SETEX", key, "30", raw)
    deadline = time.monotonic() + 30
    while not (ready(36001) and ready(36002)):
        assert time.monotonic() < deadline, "gateway readiness did not recover"
        time.sleep(.2)
    client = ChatClient()
    try:
        client.connect()
        client.login("route-proof-" + str(time.time_ns()), "")
    finally:
        client.close()
    print("PASS: restored readiness admits a real login through HAProxy")


if __name__ == "__main__":
    main()
