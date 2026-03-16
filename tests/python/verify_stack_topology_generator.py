#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
STACK_DIR = REPO_ROOT / "docker" / "stack"
BASE_COMPOSE = STACK_DIR / "docker-compose.yml"
GENERATOR_SCRIPT = REPO_ROOT / "scripts" / "generate_stack_topology.py"
DEFAULT_TOPOLOGY = STACK_DIR / "topologies" / "default.json"
PROOF_TOPOLOGY = STACK_DIR / "topologies" / "mmorpg-same-world-proof.json"
INVALID_TOPOLOGY = STACK_DIR / "topologies" / "invalid-duplicate-port.json"
TEMP_DIR = REPO_ROOT / "build" / "stack-topology-tests"


def run_command(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if expect_success and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if not expect_success and result.returncode == 0:
        raise RuntimeError(f"command unexpectedly succeeded: {' '.join(command)}")
    return result


def generate_and_validate(topology_config: Path, output_stem: str) -> None:
    TEMP_DIR.mkdir(parents=True, exist_ok=True)
    output_compose = TEMP_DIR / f"{output_stem}.generated.yml"
    output_active = TEMP_DIR / f"{output_stem}.active.json"
    run_command(
        [
            sys.executable,
            str(GENERATOR_SCRIPT),
            "--topology-config",
            str(topology_config),
            "--output-compose",
            str(output_compose),
            "--output-active",
            str(output_active),
        ],
        expect_success=True,
    )
    run_command(
        [
            "docker",
            "compose",
            "--project-name",
            "dynaxis-stack",
            "--project-directory",
            str(STACK_DIR),
            "-f",
            str(BASE_COMPOSE),
            "-f",
            str(output_compose),
            "config",
            "--quiet",
        ],
        expect_success=True,
    )


def main() -> int:
    try:
        generate_and_validate(DEFAULT_TOPOLOGY, "default")
        generate_and_validate(PROOF_TOPOLOGY, "proof")
        run_command(
            [
                sys.executable,
                str(GENERATOR_SCRIPT),
                "--topology-config",
                str(INVALID_TOPOLOGY),
                "--output-compose",
                str(TEMP_DIR / "invalid.generated.yml"),
                "--output-active",
                str(TEMP_DIR / "invalid.active.json"),
            ],
            expect_success=False,
        )
        print("PASS: stack topology generator validation")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
