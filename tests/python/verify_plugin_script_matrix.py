from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]

RUNTIME_TOGGLE_SCRIPT = Path(__file__).resolve().with_name("verify_runtime_toggle_metrics.py")
PLUGIN_HOT_RELOAD_SCRIPT = Path(__file__).resolve().with_name("verify_plugin_hot_reload.py")
PLUGIN_V2_FALLBACK_SCRIPT = Path(__file__).resolve().with_name("verify_plugin_v2_fallback.py")
PLUGIN_ROLLBACK_SCRIPT = Path(__file__).resolve().with_name("verify_plugin_rollback.py")
SCRIPT_HOT_RELOAD_SCRIPT = Path(__file__).resolve().with_name("verify_script_hot_reload.py")
SCRIPT_FALLBACK_SWITCH_SCRIPT = Path(__file__).resolve().with_name("verify_script_fallback_switch.py")
CHAT_HOOK_BEHAVIOR_SCRIPT = Path(__file__).resolve().with_name("verify_chat_hook_behavior.py")


def build_plan(scenario: str) -> list[tuple[str, list[str]]]:
    if scenario not in {"runtime-on-acceptance", "matrix"}:
        raise ValueError(f"unsupported scenario: {scenario}")

    return [
        (
            "runtime-toggles-on",
            [
                str(RUNTIME_TOGGLE_SCRIPT),
                "--expect-chat-hook-enabled",
                "1",
                "--expect-lua-enabled",
                "1",
            ],
        ),
        ("plugin-metrics-check-only", [str(PLUGIN_HOT_RELOAD_SCRIPT), "--check-only"]),
        ("plugin-hot-reload", [str(PLUGIN_HOT_RELOAD_SCRIPT)]),
        ("plugin-v2-fallback", [str(PLUGIN_V2_FALLBACK_SCRIPT)]),
        ("plugin-rollback", [str(PLUGIN_ROLLBACK_SCRIPT)]),
        ("script-hot-reload", [str(SCRIPT_HOT_RELOAD_SCRIPT)]),
        ("script-fallback-switch", [str(SCRIPT_FALLBACK_SWITCH_SCRIPT)]),
        ("chat-hook-behavior", [str(CHAT_HOOK_BEHAVIOR_SCRIPT)]),
    ]


def run_stage(name: str, command: list[str]) -> None:
    print(f"[stage] {name}")
    completed = subprocess.run(
        [sys.executable, *command],
        cwd=REPO_ROOT,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"{name} failed with exit code {completed.returncode}: {' '.join(command)}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run the runtime-on plugin and Lua proof sequence as one named acceptance matrix."
    )
    parser.add_argument(
        "--scenario",
        choices=("runtime-on-acceptance", "matrix"),
        default="runtime-on-acceptance",
    )
    args = parser.parse_args(argv)

    try:
        for stage_name, command in build_plan(args.scenario):
            run_stage(stage_name, command)
        print("PASS runtime-on-acceptance: plugin and script acceptance stages succeeded")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
