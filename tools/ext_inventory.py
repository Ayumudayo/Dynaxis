#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path


HOOK_SCOPES = {
    "on_chat_send",
    "on_login",
    "on_join",
    "on_leave",
    "on_session_event",
    "on_admin_command",
}
STAGES = {"pre_validate", "mutate", "authorize", "side_effect", "observe"}
TARGET_PROFILES = {"chat", "world", "all"}
PLUGIN_EXTENSIONS = (".so", ".dll", ".dylib", ".cpp")
CHECKSUM_PATTERN = re.compile(r"^sha256:(REPLACE_[A-Z0-9_]+|[0-9a-fA-F]{64})$")


@dataclass
class InventoryRecord:
    manifest_path: str
    kind: str | None
    artifact_path: str | None
    issues: list[str]


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def resolve_path(root: Path, raw_path: str) -> Path:
    path = Path(raw_path)
    if not path.is_absolute():
        path = root / path
    return path


def detect_expected_kind(manifest_path: Path) -> str | None:
    name = manifest_path.name
    if name.endswith(".plugin.json") or name == "plugin_manifest.template.json":
        return "native_plugin"
    if name.endswith(".script.json") or name == "script_manifest.template.json":
        return "lua_script"
    return None


def derive_artifact_path(manifest_path: Path, expected_kind: str | None) -> Path | None:
    if expected_kind is None:
        return None

    name = manifest_path.name
    if expected_kind == "native_plugin" and name.endswith(".plugin.json"):
        stem = name[: -len(".plugin.json")]
        for ext in PLUGIN_EXTENSIONS:
            candidate = manifest_path.with_name(stem + ext)
            if candidate.exists():
                return candidate
        return manifest_path.with_name(stem + ".so")

    if expected_kind == "lua_script" and name.endswith(".script.json"):
        stem = name[: -len(".script.json")]
        return manifest_path.with_name(stem + ".lua")

    return None


def validate_manifest(
    manifest: dict[str, object], expected_kind: str | None
) -> list[str]:
    issues: list[str] = []

    name = manifest.get("name")
    if not isinstance(name, str) or not name.strip():
        issues.append("name must be a non-empty string")

    version = manifest.get("version")
    if not isinstance(version, str) or not version.strip():
        issues.append("version must be a non-empty string")

    kind = manifest.get("kind")
    if not isinstance(kind, str) or kind not in {"native_plugin", "lua_script"}:
        issues.append("kind must be one of: native_plugin, lua_script")
    elif expected_kind is not None and kind != expected_kind:
        issues.append(f"kind mismatch: expected {expected_kind}, got {kind}")

    hook_scope = manifest.get("hook_scope")
    if not isinstance(hook_scope, list) or not hook_scope:
        issues.append("hook_scope must be a non-empty list")
    else:
        for idx, hook in enumerate(hook_scope):
            if not isinstance(hook, str) or hook not in HOOK_SCOPES:
                issues.append(
                    f"hook_scope[{idx}] must be one of: {', '.join(sorted(HOOK_SCOPES))}"
                )

    stage = manifest.get("stage")
    if not isinstance(stage, str) or stage not in STAGES:
        issues.append(f"stage must be one of: {', '.join(sorted(STAGES))}")

    priority = manifest.get("priority")
    if not isinstance(priority, int) or priority < 0:
        issues.append("priority must be an integer >= 0")

    exclusive_group = manifest.get("exclusive_group")
    if not isinstance(exclusive_group, str) or not exclusive_group.strip():
        issues.append("exclusive_group must be a non-empty string")

    checksum = manifest.get("checksum")
    if not isinstance(checksum, str) or CHECKSUM_PATTERN.fullmatch(checksum) is None:
        issues.append("checksum must match sha256:<64hex> or sha256:REPLACE_<TOKEN>")

    compat = manifest.get("compat")
    if not isinstance(compat, dict) or not compat:
        issues.append("compat must be a non-empty object")

    target_profiles = manifest.get("target_profiles")
    if target_profiles is not None:
        if not isinstance(target_profiles, list) or not target_profiles:
            issues.append("target_profiles must be a non-empty list when present")
        else:
            for idx, profile in enumerate(target_profiles):
                if not isinstance(profile, str) or profile not in TARGET_PROFILES:
                    issues.append(
                        f"target_profiles[{idx}] must be one of: {', '.join(sorted(TARGET_PROFILES))}"
                    )

    description = manifest.get("description")
    if description is not None and not isinstance(description, str):
        issues.append("description must be a string when present")

    owner = manifest.get("owner")
    if owner is not None and not isinstance(owner, str):
        issues.append("owner must be a string when present")

    return issues


def load_manifest(path: Path) -> dict[str, object]:
    loaded = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(loaded, dict):
        raise ValueError("manifest root must be a JSON object")
    return loaded


def validate_manifest_file(path: Path, allow_missing_artifact: bool) -> InventoryRecord:
    expected_kind = detect_expected_kind(path)
    artifact_path = derive_artifact_path(path, expected_kind)
    issues: list[str] = []

    try:
        manifest = load_manifest(path)
        issues.extend(validate_manifest(manifest, expected_kind))
    except Exception as exc:
        issues.append(f"invalid JSON manifest: {exc}")
        manifest = None

    if (
        artifact_path is not None
        and not allow_missing_artifact
        and not artifact_path.exists()
    ):
        issues.append(f"artifact not found for manifest: {artifact_path}")

    kind = None
    if manifest is not None:
        raw_kind = manifest.get("kind")
        if isinstance(raw_kind, str):
            kind = raw_kind
    if kind is None:
        kind = expected_kind

    return InventoryRecord(
        manifest_path=path.as_posix(),
        kind=kind,
        artifact_path=artifact_path.as_posix() if artifact_path is not None else None,
        issues=issues,
    )


def discover_manifests(
    plugins_dir: Path, scripts_dir: Path, include_templates: bool
) -> list[Path]:
    manifests: list[Path] = []
    if plugins_dir.exists():
        manifests.extend(plugins_dir.rglob("*.plugin.json"))
        if include_templates:
            manifests.extend(plugins_dir.rglob("plugin_manifest.template.json"))
    if scripts_dir.exists():
        manifests.extend(scripts_dir.rglob("*.script.json"))
        if include_templates:
            manifests.extend(scripts_dir.rglob("script_manifest.template.json"))
    return sorted(set(manifests))


def build_fingerprint(manifest_paths: list[Path]) -> dict[str, tuple[int, int]]:
    fingerprint: dict[str, tuple[int, int]] = {}
    for path in manifest_paths:
        key = path.as_posix()
        try:
            stat = path.stat()
            fingerprint[key] = (stat.st_mtime_ns, stat.st_size)
        except OSError:
            fingerprint[key] = (-1, -1)
    return fingerprint


def scan_records(
    manifest_paths: list[Path], allow_missing_artifact: bool
) -> tuple[list[InventoryRecord], int]:
    records: list[InventoryRecord] = []
    for manifest_path in manifest_paths:
        records.append(validate_manifest_file(manifest_path, allow_missing_artifact))
    error_count = sum(len(record.issues) for record in records)
    return records, error_count


def emit_records(
    records: list[InventoryRecord], error_count: int, json_output: bool
) -> None:
    if json_output:
        payload = {
            "manifest_count": len(records),
            "error_count": error_count,
            "records": [
                {
                    "manifest_path": record.manifest_path,
                    "kind": record.kind,
                    "artifact_path": record.artifact_path,
                    "issues": record.issues,
                }
                for record in records
            ],
        }
        print(json.dumps(payload, indent=2))
        return

    if not records:
        print("[ext_inventory] no manifests found")
        return

    print(f"[ext_inventory] scanned manifests: {len(records)}")
    for record in records:
        if record.issues:
            print(f"[ext_inventory] FAIL {record.manifest_path}")
            for issue in record.issues:
                print(f"  - {issue}")
        else:
            print(f"[ext_inventory] OK   {record.manifest_path}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Scan extensibility manifests and validate schema rules."
    )
    parser.add_argument(
        "--plugins-dir",
        default="server/plugins",
        help="Directory containing plugin artifacts",
    )
    parser.add_argument(
        "--scripts-dir",
        default="server/scripts",
        help="Directory containing lua scripts",
    )
    parser.add_argument(
        "--manifest", action="append", help="Explicit manifest file path (repeatable)"
    )
    parser.add_argument(
        "--include-templates",
        action="store_true",
        help="Include *.template.json manifests",
    )
    parser.add_argument(
        "--allow-missing-artifact",
        action="store_true",
        help="Skip artifact existence checks",
    )
    parser.add_argument(
        "--json", action="store_true", help="Emit machine-readable JSON output"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Return non-zero if validation errors are found",
    )
    parser.add_argument(
        "--watch-interval-ms",
        type=int,
        default=0,
        help="Polling interval in milliseconds",
    )
    parser.add_argument(
        "--watch-iterations",
        type=int,
        default=0,
        help="Watch loop iterations (0 means infinite when watch is enabled)",
    )
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.watch_interval_ms < 0:
        parser.error("--watch-interval-ms must be >= 0")
    if args.watch_iterations < 0:
        parser.error("--watch-iterations must be >= 0")

    root = repo_root()
    explicit_manifests = [resolve_path(root, raw) for raw in (args.manifest or [])]
    plugins_dir = resolve_path(root, args.plugins_dir)
    scripts_dir = resolve_path(root, args.scripts_dir)

    watch_enabled = args.watch_interval_ms > 0
    max_error_count = 0
    previous_fingerprint: dict[str, tuple[int, int]] | None = None

    iteration = 0
    while True:
        if explicit_manifests:
            manifest_paths = explicit_manifests
        else:
            manifest_paths = discover_manifests(
                plugins_dir, scripts_dir, args.include_templates
            )

        current_fingerprint = build_fingerprint(manifest_paths)
        should_emit = (
            previous_fingerprint is None or current_fingerprint != previous_fingerprint
        )

        if should_emit:
            records, error_count = scan_records(
                manifest_paths, args.allow_missing_artifact
            )
            max_error_count = max(max_error_count, error_count)
            emit_records(records, error_count, args.json)
            previous_fingerprint = current_fingerprint

        if not watch_enabled:
            break

        iteration += 1
        if args.watch_iterations > 0 and iteration >= args.watch_iterations:
            break
        time.sleep(args.watch_interval_ms / 1000.0)

    if args.check and max_error_count > 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
