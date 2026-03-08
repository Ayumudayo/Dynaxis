import os
import subprocess
import time
from pathlib import Path


def _env_list(name: str, default_csv: str) -> list[str]:
    raw = os.getenv(name, default_csv)
    items = [part.strip() for part in raw.split(",")]
    return [item for item in items if item]


REPO_ROOT = Path(__file__).resolve().parents[2]
PROBE_SCRIPT = Path(
    os.getenv(
        "LUA_PROBE_SCRIPT", str(REPO_ROOT / "docker/stack/scripts/hot_reload_probe.lua")
    )
)
SERVER_CONTAINERS = _env_list(
    "LUA_SERVER_CONTAINERS",
    "dynaxis-stack-server-1-1,dynaxis-stack-server-2-1",
)

WAIT_TIMEOUT_SEC = float(os.getenv("LUA_WAIT_TIMEOUT_SEC", "30"))
POLL_INTERVAL_SEC = float(os.getenv("LUA_POLL_INTERVAL_SEC", "0.5"))
LOG_WINDOW = os.getenv("LUA_LOG_WINDOW", "10m")

MARKER_DETECTED = "Lua script watcher detected changes"
MARKER_RELOADED = "Lua script reload complete"

DEFAULT_PROBE = """return {
  hook = "on_login",
  decision = "pass",
  notice = "hot reload probe"
}
"""


def run_cmd(cmd: list[str]) -> str:
    try:
        proc = subprocess.run(cmd, check=True, capture_output=True, text=True)
        return (proc.stdout or "") + (("\n" + proc.stderr) if proc.stderr else "")
    except subprocess.CalledProcessError as exc:
        output = (exc.stdout or "") + ("\n" + exc.stderr if exc.stderr else "")
        raise RuntimeError(
            f"command failed: {' '.join(cmd)}\n{output.strip()}"
        ) from exc


def read_log_markers(container: str) -> tuple[int, int]:
    text = run_cmd(["docker", "logs", container, "--since", LOG_WINDOW])
    return text.count(MARKER_DETECTED), text.count(MARKER_RELOADED)


def toggle_probe_notice(text: str) -> str:
    if "hot reload probe v2" in text:
        return text.replace("hot reload probe v2", "hot reload probe", 1)
    if "hot reload probe" in text:
        return text.replace("hot reload probe", "hot reload probe v2", 1)
    stamp = int(time.time() * 1000)
    suffix = "" if text.endswith("\n") else "\n"
    return text + suffix + f"-- hot-reload-probe:{stamp}\n"


def wait_for_reload_events(baseline: dict[str, tuple[int, int]]) -> None:
    deadline = time.time() + WAIT_TIMEOUT_SEC
    last_pending: list[str] = []
    while time.time() < deadline:
        pending: list[str] = []
        for container, (base_detected, base_reloaded) in baseline.items():
            detected, reloaded = read_log_markers(container)
            if detected <= base_detected or reloaded <= base_reloaded:
                pending.append(
                    f"{container}(detected={detected}/{base_detected},reloaded={reloaded}/{base_reloaded})"
                )

        if not pending:
            return

        last_pending = pending
        time.sleep(POLL_INTERVAL_SEC)

    raise RuntimeError(
        "lua watcher/reload markers did not advance: " + ", ".join(last_pending)
    )


def main() -> int:
    if not SERVER_CONTAINERS:
        print("FAIL: no LUA_SERVER_CONTAINERS configured")
        return 1

    created_probe = not PROBE_SCRIPT.exists()
    original = (
        DEFAULT_PROBE if created_probe else PROBE_SCRIPT.read_text(encoding="utf-8")
    )
    updated = toggle_probe_notice(original)

    baseline: dict[str, tuple[int, int]] = {}

    try:
        PROBE_SCRIPT.parent.mkdir(parents=True, exist_ok=True)

        for container in SERVER_CONTAINERS:
            baseline[container] = read_log_markers(container)

        PROBE_SCRIPT.write_text(updated, encoding="utf-8")
        wait_for_reload_events(baseline)

        print("PASS: lua script hot-reload markers observed on all server containers")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        try:
            if created_probe:
                PROBE_SCRIPT.unlink(missing_ok=True)
            else:
                PROBE_SCRIPT.write_text(original, encoding="utf-8")
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
