import os
import shutil
import subprocess
import time
from pathlib import Path


def _env_list(name: str, default_csv: str) -> list[str]:
    raw = os.getenv(name, default_csv)
    items = [part.strip() for part in raw.split(",")]
    return [item for item in items if item]


REPO_ROOT = Path(__file__).resolve().parents[2]
PRIMARY_SCRIPTS_DIR = Path(
    os.getenv("LUA_PRIMARY_SCRIPTS_DIR", str(REPO_ROOT / "docker/stack/scripts"))
)
SERVER_CONTAINERS = _env_list(
    "LUA_SERVER_CONTAINERS",
    "dynaxis-stack-server-1-1,dynaxis-stack-server-2-1",
)

WAIT_TIMEOUT_SEC = float(os.getenv("LUA_WAIT_TIMEOUT_SEC", "45"))
POLL_INTERVAL_SEC = float(os.getenv("LUA_POLL_INTERVAL_SEC", "0.5"))
LOG_WINDOW = os.getenv("LUA_LOG_WINDOW", "10m")

FALLBACK_MARKER = "Lua scripts fallback selected scripts_dir=/app/scripts_builtin"
PRIMARY_SWITCH_MARKER = (
    "Lua scripts source selected scripts_dir=/app/scripts reason=switch"
)

PROBE_FILE_NAME = "fallback_switch_probe.lua"
PROBE_SCRIPT = """return {
  hook = "on_login",
  decision = "pass",
  notice = "fallback switch probe"
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


def read_marker_count(container: str, marker: str) -> int:
    text = run_cmd(["docker", "logs", container, "--since", LOG_WINDOW])
    return text.count(marker)


def wait_for_marker_advance(
    marker: str, baseline: dict[str, int], timeout_sec: float
) -> None:
    deadline = time.time() + timeout_sec
    last_pending: list[str] = []
    while time.time() < deadline:
        pending: list[str] = []
        for container, base_count in baseline.items():
            current = read_marker_count(container, marker)
            if current <= base_count:
                pending.append(f"{container}({current}/{base_count})")

        if not pending:
            return

        last_pending = pending
        time.sleep(POLL_INTERVAL_SEC)

    raise RuntimeError(
        f"marker did not advance marker={marker} pending={', '.join(last_pending)}"
    )


def main() -> int:
    if not SERVER_CONTAINERS:
        print("FAIL: no LUA_SERVER_CONTAINERS configured")
        return 1
    if not PRIMARY_SCRIPTS_DIR.exists():
        print(f"FAIL: primary scripts dir not found: {PRIMARY_SCRIPTS_DIR}")
        return 1

    ts = int(time.time() * 1000)
    backup_dir = PRIMARY_SCRIPTS_DIR.parent / f".fallback_switch_backup_{ts}"
    backup_dir.mkdir(parents=True, exist_ok=True)

    moved_files: list[Path] = []
    probe_path = PRIMARY_SCRIPTS_DIR / PROBE_FILE_NAME

    fallback_baseline: dict[str, int] = {}
    primary_baseline: dict[str, int] = {}

    try:
        for script in PRIMARY_SCRIPTS_DIR.glob("*.lua"):
            target = backup_dir / script.name
            shutil.move(str(script), str(target))
            moved_files.append(script)

        for container in SERVER_CONTAINERS:
            fallback_baseline[container] = read_marker_count(container, FALLBACK_MARKER)

        wait_for_marker_advance(FALLBACK_MARKER, fallback_baseline, WAIT_TIMEOUT_SEC)

        for container in SERVER_CONTAINERS:
            primary_baseline[container] = read_marker_count(
                container, PRIMARY_SWITCH_MARKER
            )

        probe_path.write_text(PROBE_SCRIPT, encoding="utf-8")

        wait_for_marker_advance(
            PRIMARY_SWITCH_MARKER, primary_baseline, WAIT_TIMEOUT_SEC
        )

        print(
            "PASS: lua scripts fallback<->primary switching observed on all server containers"
        )
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        try:
            probe_path.unlink(missing_ok=True)
        except Exception:
            pass

        for original_path in moved_files:
            backup_path = backup_dir / original_path.name
            if not backup_path.exists():
                continue
            try:
                shutil.move(str(backup_path), str(original_path))
            except Exception:
                pass

        try:
            backup_dir.rmdir()
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
