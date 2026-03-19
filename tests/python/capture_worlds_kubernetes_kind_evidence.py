from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ARTIFACT_ROOT = REPO_ROOT / "build" / "k8s-kind-evidence"
PROOF_ROOT = Path(__file__).resolve().parent


def default_run_id() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%SZ")


def proof_specs(capture_set: str) -> list[tuple[str, list[str]]]:
    baseline = [
        ("localdev-manifest", ["verify_worlds_kubernetes_localdev.py"]),
        ("kind-live", ["verify_worlds_kubernetes_kind.py"]),
        ("kind-control-plane", ["verify_worlds_kubernetes_kind_control_plane.py"]),
        ("kind-closure", ["verify_worlds_kubernetes_kind_closure.py"]),
        ("kind-continuity", ["verify_worlds_kubernetes_kind_continuity.py"]),
        ("kind-multigateway", ["verify_worlds_kubernetes_kind_multigateway.py"]),
        ("kind-restart", ["verify_worlds_kubernetes_kind_restart.py"]),
        (
            "kind-locator-fallback",
            [
                "verify_worlds_kubernetes_kind_locator_fallback.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-locator-fallback",
            ],
        ),
        (
            "kind-world-state-fallback",
            [
                "verify_worlds_kubernetes_kind_world_state_fallback.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-world-state-fallback",
            ],
        ),
        (
            "kind-redis-outage",
            [
                "verify_worlds_kubernetes_kind_redis_outage.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-redis-outage",
            ],
        ),
        (
            "kind-worker-outage",
            [
                "verify_worlds_kubernetes_kind_worker_outage.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-worker-outage",
            ],
        ),
        (
            "kind-gateway-ingress-impairment",
            [
                "verify_worlds_kubernetes_kind_gateway_ingress_impairment.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-gateway-ingress-impairment",
            ],
        ),
        (
            "kind-resume-impairment",
            [
                "verify_worlds_kubernetes_kind_resume_impairment.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-resume-impairment",
            ],
        ),
        (
            "kind-multifault-impairment",
            [
                "verify_worlds_kubernetes_kind_multifault_impairment.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-multifault-impairment",
            ],
        ),
        (
            "kind-metrics-budget",
            [
                "verify_worlds_kubernetes_kind_metrics_budget.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-metrics-budget",
            ],
        ),
    ]
    resume_only = [
        ("kind-continuity", ["verify_worlds_kubernetes_kind_continuity.py"]),
        ("kind-multigateway", ["verify_worlds_kubernetes_kind_multigateway.py"]),
        ("kind-restart", ["verify_worlds_kubernetes_kind_restart.py"]),
        (
            "kind-locator-fallback",
            [
                "verify_worlds_kubernetes_kind_locator_fallback.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-locator-fallback",
            ],
        ),
        (
            "kind-world-state-fallback",
            [
                "verify_worlds_kubernetes_kind_world_state_fallback.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-world-state-fallback",
            ],
        ),
    ]
    metrics_only = [
        (
            "kind-metrics-budget",
            [
                "verify_worlds_kubernetes_kind_metrics_budget.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-metrics-budget",
            ],
        )
    ]
    fallback_only = [
        (
            "kind-locator-fallback",
            [
                "verify_worlds_kubernetes_kind_locator_fallback.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-locator-fallback",
            ],
        )
    ]
    outage_only = [
        (
            "kind-redis-outage",
            [
                "verify_worlds_kubernetes_kind_redis_outage.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-redis-outage",
            ],
        )
    ]
    worker_outage_only = [
        (
            "kind-worker-outage",
            [
                "verify_worlds_kubernetes_kind_worker_outage.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-worker-outage",
            ],
        )
    ]
    impairment_only = [
        (
            "kind-gateway-ingress-impairment",
            [
                "verify_worlds_kubernetes_kind_gateway_ingress_impairment.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-gateway-ingress-impairment",
            ],
        )
    ]
    resume_impairment_only = [
        (
            "kind-resume-impairment",
            [
                "verify_worlds_kubernetes_kind_resume_impairment.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-resume-impairment",
            ],
        )
    ]
    multifault_only = [
        (
            "kind-multifault-impairment",
            [
                "verify_worlds_kubernetes_kind_multifault_impairment.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-multifault-impairment",
            ],
        )
    ]
    world_state_only = [
        (
            "kind-world-state-fallback",
            [
                "verify_worlds_kubernetes_kind_world_state_fallback.py",
                "--artifact-dir",
                "__ARTIFACT_DIR__/kind-world-state-fallback",
            ],
        )
    ]
    mapping = {
        "baseline": baseline,
        "resume-only": resume_only,
        "metrics-only": metrics_only,
        "fallback-only": fallback_only,
        "outage-only": outage_only,
        "worker-outage-only": worker_outage_only,
        "impairment-only": impairment_only,
        "resume-impairment-only": resume_impairment_only,
        "multifault-only": multifault_only,
        "world-state-only": world_state_only,
    }
    return mapping[capture_set]


def run_proof(name: str, script_args: list[str], artifact_root: Path) -> dict[str, object]:
    resolved_args = [
        str(arg).replace("__ARTIFACT_DIR__", str(artifact_root))
        for arg in script_args
    ]
    script_path = PROOF_ROOT / resolved_args[0]
    command = [sys.executable, str(script_path), *resolved_args[1:]]
    log_path = artifact_root / f"{name}.log"

    started_at = datetime.now(timezone.utc).isoformat()
    started_monotonic = time.monotonic()
    completed = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    duration_seconds = round(time.monotonic() - started_monotonic, 3)

    combined = completed.stdout or ""
    if completed.stderr:
        if combined and not combined.endswith("\n"):
            combined += "\n"
        combined += completed.stderr
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(combined, encoding="utf-8")

    verdict = "pass" if completed.returncode == 0 else "fail"
    if completed.returncode == 77:
        verdict = "skip"

    return {
        "name": name,
        "script": str(script_path.relative_to(REPO_ROOT)),
        "args": resolved_args[1:],
        "command": command,
        "started_at_utc": started_at,
        "duration_seconds": duration_seconds,
        "return_code": completed.returncode,
        "verdict": verdict,
        "log_path": str(log_path.relative_to(REPO_ROOT)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Capture one repeatable artifact set for the live kind Kubernetes proof suite."
    )
    parser.add_argument("--run-id", default=default_run_id())
    parser.add_argument(
        "--capture-set",
        choices=("baseline", "resume-only", "metrics-only", "fallback-only", "world-state-only", "outage-only", "worker-outage-only", "impairment-only", "resume-impairment-only", "multifault-only"),
        default="baseline",
        help="Select which subset of the kind proof suite to capture.",
    )
    args = parser.parse_args()

    artifact_root = ARTIFACT_ROOT / args.run_id
    artifact_root.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, object]] = []
    total_started = time.monotonic()
    started_at = datetime.now(timezone.utc).isoformat()

    try:
        for name, script_args in proof_specs(args.capture_set):
            print(f"[k8s-kind-evidence] running {name}...")
            result = run_proof(name, script_args, artifact_root)
            results.append(result)
            if result["verdict"] == "fail":
                raise RuntimeError(f"{name} failed; see {result['log_path']}")
            print(
                f"[k8s-kind-evidence] {name}: {result['verdict']} "
                f"({result['duration_seconds']:.3f}s)"
            )
    except Exception as exc:
        status = "fail"
        error_message = str(exc)
    else:
        status = "pass"
        error_message = ""

    manifest = {
        "run_id": args.run_id,
        "capture_set": args.capture_set,
        "started_at_utc": started_at,
        "duration_seconds": round(time.monotonic() - total_started, 3),
        "status": status,
        "error_message": error_message,
        "proofs": results,
    }
    manifest_path = artifact_root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    if status != "pass":
        print(f"FAIL k8s-kind-evidence: {error_message}")
        print(f"manifest: {manifest_path}")
        return 1

    print(f"PASS k8s-kind-evidence: manifest={manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
