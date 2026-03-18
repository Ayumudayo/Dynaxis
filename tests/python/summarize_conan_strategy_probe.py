#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import pathlib
from datetime import datetime, timezone


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize Conan current-cache strategy telemetry into one JSON artifact."
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--target", default="")
    parser.add_argument("--run-ctest", required=True)
    parser.add_argument("--strategy-mode", default="current-cache")
    parser.add_argument("--restore-cache-hit", default="")
    parser.add_argument("--restore-matched-key", default="")
    parser.add_argument("--restore-primary-key", default="")
    parser.add_argument("--restore-elapsed-sec", type=float, default=None)
    parser.add_argument("--build-elapsed-sec", type=float, default=None)
    parser.add_argument("--ctest-elapsed-sec", type=float, default=None)
    parser.add_argument("--save-status", default="")
    parser.add_argument("--save-elapsed-sec", type=float, default=None)
    parser.add_argument("--save-skipped-reason", default="")
    parser.add_argument("--run-id", default="")
    parser.add_argument("--run-attempt", default="")
    parser.add_argument("--runner-os", default="")
    parser.add_argument("--workflow", default="conan2-poc")
    parser.add_argument("--job", default="windows-conan2-poc")
    parser.add_argument("--step-summary", default="")
    return parser.parse_args()


def append_summary(path_value: str, output_path: pathlib.Path, payload: dict[str, object]) -> None:
    if not path_value:
        return
    path = pathlib.Path(path_value)
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "### Conan strategy telemetry artifact",
        f"- summary_json: {output_path.as_posix()}",
        f"- strategy_mode: {payload['strategy']['mode']}",
    ]
    restore = payload["restore"]
    build = payload["build"]
    test = payload["test"]
    if isinstance(restore, dict) and restore.get("elapsed_sec") is not None:
        lines.append(f"- restore_elapsed_sec: {restore['elapsed_sec']}")
    if isinstance(build, dict) and build.get("elapsed_sec") is not None:
        lines.append(f"- build_elapsed_sec: {build['elapsed_sec']}")
    if isinstance(test, dict) and test.get("executed") and test.get("elapsed_sec") is not None:
        lines.append(f"- ctest_elapsed_sec: {test['elapsed_sec']}")
    with path.open("a", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    output_path = pathlib.Path(args.output)
    payload: dict[str, object] = {
        "workflow": args.workflow,
        "job": args.job,
        "run_id": args.run_id,
        "run_attempt": args.run_attempt,
        "runner_os": args.runner_os,
        "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
        "strategy": {
            "mode": args.strategy_mode,
            "comparison_surface": "windows-conan-build-path",
            "current_cache_contract": {
                "conan_home_cache_restore": True,
                "build_step_includes_setup_conan": True,
                "ctest_optional": True,
                "conan_home_cache_save": True,
            },
            "future_binary_remote_contract": {
                "same_config_target_surface": True,
                "same_restore_build_ctest_save_fields": True,
                "remote_rollout_present": False,
            },
        },
        "build_request": {
            "config": args.config,
            "target": args.target,
            "run_ctest": args.run_ctest,
        },
        "restore": {
            "cache_hit": args.restore_cache_hit,
            "matched_key": args.restore_matched_key,
            "primary_key": args.restore_primary_key,
            "elapsed_sec": args.restore_elapsed_sec,
        },
        "build": {
            "elapsed_sec": args.build_elapsed_sec,
        },
        "test": {
            "executed": args.run_ctest != "false" and args.target == "",
            "elapsed_sec": args.ctest_elapsed_sec,
        },
        "save": {
            "status": args.save_status,
            "elapsed_sec": args.save_elapsed_sec,
            "skipped_reason": args.save_skipped_reason,
        },
        "notes": [
            "This artifact frames the current Conan cache strategy only.",
            "A future binary-remote comparison should reuse the same JSON surface before any rollout decision.",
        ],
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    append_summary(args.step_summary, output_path, payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
