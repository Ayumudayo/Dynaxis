"""실제 DB/Redis 장애에서 readiness 하강과 앱 재시작 없는 복구·적재·fanout을 검증한다."""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
APPS = ("server-1", "server-2", "gateway-1", "gateway-2", "wb_worker")


def command(*args: str, input: str | None = None) -> str:
    result = subprocess.run(args, cwd=ROOT, input=input, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=120)
    if result.returncode:
        raise RuntimeError("dependency recovery command failed: " + args[0])
    return result.stdout.strip()


def deploy() -> None:
    command("pwsh", "-NoProfile", "-File", str(ROOT / "scripts/deploy_docker.ps1"),
            "-Action", "up", "-Detached", "-NoBase")


def wait(predicate, label: str, timeout: float = 90) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, RuntimeError):
            pass
        time.sleep(0.5)
    raise AssertionError(label + " timeout")


def ready(port: int) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/readyz", timeout=2) as response:
            return response.status == 200 and response.read().strip() == b"ready"
    except OSError:
        return False


def app_runs() -> dict:
    return {name: command("docker", "inspect", "--format", "{{.Id}} {{.State.StartedAt}}",
                          f"dynaxis-stack-{name}-1") for name in APPS}


def emit(label: str) -> str:
    event = command("docker", "exec", "dynaxis-stack-redis-1", "redis-cli", "XADD", "session_events", "*",
        "type", "session_login", "ts_ms", str(int(time.time() * 1000)),
        "session_id", "00000000-0000-0000-0000-000000000001", "payload", json.dumps({"proof": label}))
    if not re.fullmatch(r"\d+-\d+", event):
        raise AssertionError("invalid stream event id")
    return event


def persisted(event: str) -> bool:
    output = command("docker", "exec", "-i", "dynaxis-stack-postgres-1", "sh", "-lc",
        'PGPASSWORD="$POSTGRES_PASSWORD" psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -tA',
        input=f"select count(*) from session_events where event_id = '{event}';\n")
    return output == "1"


def main() -> int:
    ports = (36001, 36002, 39091, 39092, 39093)
    wait(lambda: all(ready(port) for port in ports), "initial readiness")
    original = app_runs()
    try:
        command("docker", "stop", "-t", "3", "dynaxis-stack-postgres-1")
        pending = emit("postgres-outage")
        wait(lambda: not ready(39093) and not ready(39091), "DB readiness must fall", 35)
        deploy()
        wait(lambda: all(ready(port) for port in ports), "DB recovery readiness")
        wait(lambda: persisted(pending), "pending DB outage event persistence")
        assert app_runs() == original, "DB recovery restarted an application"
        print("PASS: DB outage lowers readiness and pending event is persisted after recovery without app restart")

        command("docker", "stop", "-t", "3", "dynaxis-stack-redis-1")
        wait(lambda: not ready(39093) and not ready(36001) and not ready(39091), "Redis readiness must fall", 35)
        deploy()
        wait(lambda: all(ready(port) for port in ports), "Redis recovery readiness")
        event = emit("redis-recovery")
        wait(lambda: persisted(event), "Redis recovery event persistence")
        assert app_runs() == original, "Redis recovery restarted an application"
        command(sys.executable, str(ROOT / "tests/python/verify_whisper_cross_instance.py"))
        print("PASS: Redis outage lowers readiness; resubscription, cross-node delivery and persistence recover without app restart")
        return 0
    finally:
        deploy()


if __name__ == "__main__":
    raise SystemExit(main())
