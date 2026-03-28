#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEPLOY_SCRIPT = REPO_ROOT / "scripts" / "deploy_docker.ps1"


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def require_absent(text: str, needle: str, message: str) -> None:
    if needle in text:
        raise AssertionError(message)


def main() -> int:
    script = DEPLOY_SCRIPT.read_text(encoding="utf-8")

    require(
        script,
        "function Build-RuntimeImages",
        "deploy_docker.ps1 must define a Build-RuntimeImages helper",
    )
    require(
        script,
        "Build-RuntimeImages -ProjectRoot $ProjectRoot -NoCache:$NoCache",
        "deploy_docker.ps1 should route build-enabled paths through Build-RuntimeImages",
    )

    for image_name in (
        "dynaxis-server:local",
        "dynaxis-gateway:local",
        "dynaxis-worker:local",
        "dynaxis-admin:local",
        "dynaxis-migrator:local",
    ):
        require(
            script,
            image_name,
            f"deploy_docker.ps1 build plan is missing {image_name}",
        )

    require_absent(
        script,
        'if ($Build) { $DockerArgs += "--build" }',
        "deploy_docker.ps1 should not delegate duplicate-image runtime builds to `compose up --build`",
    )

    print("PASS: deploy_docker.ps1 prebuilds unique runtime images and avoids compose up --build")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
