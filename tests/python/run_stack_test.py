#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import cast


def _is_enabled() -> bool:
    value = os.environ.get("KNIGHTS_ENABLE_STACK_PYTHON_TESTS", "")
    return value.strip().lower() in {"1", "true", "yes", "on"}


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run a stack-dependent Python test script for ctest. "
            "Returns 77 when KNIGHTS_ENABLE_STACK_PYTHON_TESTS is not enabled."
        )
    )
    _ = parser.add_argument("script", help="Python test script path")
    _ = parser.add_argument(
        "script_args", nargs=argparse.REMAINDER, help="Arguments for the script"
    )
    args = parser.parse_args()
    script = cast(str, args.script)
    script_args = cast(list[str], args.script_args)

    if not _is_enabled():
        print("[skip] KNIGHTS_ENABLE_STACK_PYTHON_TESTS is not enabled.")
        return 77

    script_path = Path(script)
    if not script_path.exists():
        print(f"[error] script not found: {script_path}")
        return 2

    command = [sys.executable, str(script_path), *script_args]
    completed = subprocess.run(command, check=False)
    return int(completed.returncode)


if __name__ == "__main__":
    raise SystemExit(main())
