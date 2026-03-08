import argparse
import os
import subprocess
import time
import urllib.request


def _env_list(name: str, default_csv: str) -> list[str]:
    raw = os.getenv(name, default_csv)
    items = [part.strip() for part in raw.split(",")]
    return [item for item in items if item]


METRICS_URLS = _env_list(
    "PLUGIN_METRICS_URLS",
    "http://127.0.0.1:39091/metrics,http://127.0.0.1:39092/metrics",
)
SERVER_CONTAINERS = _env_list(
    "PLUGIN_SERVER_CONTAINERS",
    "dynaxis-stack-server-1-1,dynaxis-stack-server-2-1",
)

ACTIVE_PLUGIN_PATH = os.getenv(
    "PLUGIN_ACTIVE_PATH", "/app/plugins_builtin/10_chat_hook_sample.so"
)
STAGING_V1_PATH = os.getenv(
    "PLUGIN_STAGING_V1_PATH", "/app/plugins_builtin/staging/10_chat_hook_sample_v1.so"
)
STAGING_V2_PATH = os.getenv(
    "PLUGIN_STAGING_V2_PATH", "/app/plugins_builtin/staging/10_chat_hook_sample_v2.so"
)
LOCK_PATH = os.getenv(
    "PLUGIN_LOCK_PATH", "/app/plugins_builtin/10_chat_hook_sample_LOCK"
)

TARGET_FILE_LABEL = 'file="10_chat_hook_sample.so"'
WAIT_TIMEOUT_SEC = float(os.getenv("PLUGIN_WAIT_TIMEOUT_SEC", "30"))
POLL_INTERVAL_SEC = float(os.getenv("PLUGIN_POLL_INTERVAL_SEC", "0.5"))


def fetch_metrics(url: str) -> str:
    with urllib.request.urlopen(url, timeout=5) as response:
        return response.read().decode("utf-8", errors="replace")


def check_plugin_metrics_presence() -> None:
    for url in METRICS_URLS:
        text = fetch_metrics(url)
        if "chat_hook_plugins_enabled" not in text:
            raise RuntimeError(f"missing chat_hook_plugins_enabled in {url}")
        if "chat_hook_plugin_info" not in text:
            raise RuntimeError(f"missing chat_hook_plugin_info in {url}")


def run_docker_exec(container: str, script: str) -> None:
    cmd = ["docker", "exec", container, "sh", "-lc", script]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as exc:
        output = (exc.stdout or "") + ("\n" + exc.stderr if exc.stderr else "")
        raise RuntimeError(
            f"docker exec failed for {container}: {output.strip()}"
        ) from exc


def apply_v2_swap() -> None:
    swap_script = (
        "set -e; "
        f"mkdir -p \"$(dirname '{STAGING_V1_PATH}')\"; "
        f'cp "{ACTIVE_PLUGIN_PATH}" "{STAGING_V1_PATH}"; '
        f'touch "{LOCK_PATH}"; '
        f'cp "{STAGING_V2_PATH}" "{ACTIVE_PLUGIN_PATH}"; '
        f'rm -f "{LOCK_PATH}"'
    )
    for container in SERVER_CONTAINERS:
        run_docker_exec(container, swap_script)


def wait_for_version(version: str) -> None:
    deadline = time.time() + WAIT_TIMEOUT_SEC
    expected_version = f'version="{version}"'

    while time.time() < deadline:
        all_ready = True
        for url in METRICS_URLS:
            text = fetch_metrics(url)
            if TARGET_FILE_LABEL not in text or expected_version not in text:
                all_ready = False
                break

        if all_ready:
            return

        time.sleep(POLL_INTERVAL_SEC)

    raise RuntimeError(f"plugin version did not converge to {version} before timeout")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify plugin metrics and v2 hot-reload"
    )
    parser.add_argument(
        "--check-only", action="store_true", help="only verify plugin metrics presence"
    )
    args = parser.parse_args()

    try:
        check_plugin_metrics_presence()
        print("plugin metrics present")

        if args.check_only:
            print("PASS: plugin metrics check-only")
            return 0

        apply_v2_swap()
        wait_for_version("v2")
        print("PASS: plugin hot-reload to v2 reflected in metrics")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
