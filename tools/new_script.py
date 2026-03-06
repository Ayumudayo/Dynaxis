#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


HOOK_CHOICES = (
    "on_login",
    "on_join",
    "on_leave",
    "on_session_event",
    "on_admin_command",
    "on_chat_send",
)
DECISION_CHOICES = ("pass", "allow", "modify", "handled", "block", "deny")
STAGE_CHOICES = ("pre_validate", "mutate", "authorize", "side_effect", "observe")
TARGET_PROFILE_CHOICES = ("chat", "world", "all")

SCRIPT_TEMPLATE_REL = Path("server/scripts/templates/on_join_template.lua")
MANIFEST_TEMPLATE_REL = Path("server/scripts/templates/script_manifest.template.json")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def sanitize_name(raw: str) -> str:
    token = raw.strip()
    if token.endswith(".lua"):
        token = token[:-4]
    token = re.sub(r"[^A-Za-z0-9_]+", "_", token)
    token = token.strip("_").lower()
    if not token:
        raise ValueError("--name must contain at least one alphanumeric character")
    if token[0].isdigit():
        token = f"s_{token}"
    return token


def normalize_profiles(raw_profiles: list[str] | None) -> list[str]:
    profiles = list(raw_profiles or ["all"])
    deduped: list[str] = []
    seen: set[str] = set()
    for profile in profiles:
        if profile in seen:
            continue
        deduped.append(profile)
        seen.add(profile)
    if "all" in seen:
        return ["all"]
    return deduped


def resolve_output_dir(root: Path, raw_dir: str) -> Path:
    path = Path(raw_dir)
    if not path.is_absolute():
        path = root / path
    return path


def ensure_writable(path: Path, force: bool) -> None:
    if path.exists() and not force:
        raise FileExistsError(f"refusing to overwrite existing file: {path}")


def render_script(
    template_text: str, hook: str, decision: str, reason: str, notice: str
) -> str:
    rendered = template_text
    rendered = rendered.replace("__HOOK_NAME__", hook)
    rendered = rendered.replace("__DECISION__", decision)
    rendered = rendered.replace("__REASON__", reason)
    rendered = rendered.replace("__NOTICE__", notice)
    if not rendered.endswith("\n"):
        rendered += "\n"
    return rendered


def load_manifest_template(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def apply_manifest_values(
    manifest: dict[str, object],
    *,
    script_name: str,
    version: str,
    hook: str,
    stage: str,
    priority: int,
    exclusive_group: str,
    target_profiles: list[str],
    description: str | None,
    owner: str | None,
) -> dict[str, object]:
    manifest["name"] = script_name
    manifest["version"] = version
    manifest["hook_scope"] = [hook]
    manifest["stage"] = stage
    manifest["priority"] = priority
    manifest["exclusive_group"] = exclusive_group
    manifest["target_profiles"] = target_profiles
    if description:
        manifest["description"] = description
    if owner:
        manifest["owner"] = owner
    return manifest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create a Lua cold-hook script scaffold."
    )
    parser.add_argument(
        "--name", required=True, help="Script name (file stem and manifest name)"
    )
    parser.add_argument(
        "--hook",
        choices=HOOK_CHOICES,
        default="on_join",
        help="Hook name for the script",
    )
    parser.add_argument(
        "--decision",
        choices=DECISION_CHOICES,
        default="pass",
        help="Template decision value",
    )
    parser.add_argument(
        "--reason",
        default="TODO: fill deny/block reason when applicable",
        help="Template reason",
    )
    parser.add_argument(
        "--notice", default="TODO: optional notice", help="Template notice"
    )
    parser.add_argument("--version", default="0.1.0", help="Script version string")
    parser.add_argument(
        "--stage",
        choices=STAGE_CHOICES,
        default="authorize",
        help="Manifest stage value",
    )
    parser.add_argument(
        "--priority", type=int, default=30, help="Manifest priority value (>= 0)"
    )
    parser.add_argument("--exclusive-group", help="Manifest exclusive_group value")
    parser.add_argument(
        "--target-profile",
        action="append",
        choices=TARGET_PROFILE_CHOICES,
        help="Target profile in manifest (repeatable)",
    )
    parser.add_argument("--description", help="Manifest description override")
    parser.add_argument("--owner", help="Manifest owner override")
    parser.add_argument(
        "--output-dir",
        default="server/scripts",
        help="Output directory for scaffold files",
    )
    parser.add_argument("--force", action="store_true", help="Overwrite existing files")
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.priority < 0:
        parser.error("--priority must be >= 0")

    root = repo_root()
    script_template_path = root / SCRIPT_TEMPLATE_REL
    manifest_template_path = root / MANIFEST_TEMPLATE_REL
    output_dir = resolve_output_dir(root, args.output_dir)

    script_name = sanitize_name(args.name)
    target_profiles = normalize_profiles(args.target_profile)
    exclusive_group = args.exclusive_group or script_name

    script_path = output_dir / f"{script_name}.lua"
    manifest_path = output_dir / f"{script_name}.script.json"

    try:
        script_template = script_template_path.read_text(encoding="utf-8")
        manifest_template = load_manifest_template(manifest_template_path)

        output_dir.mkdir(parents=True, exist_ok=True)
        ensure_writable(script_path, args.force)
        ensure_writable(manifest_path, args.force)

        script_text = render_script(
            script_template, args.hook, args.decision, args.reason, args.notice
        )
        script_path.write_text(script_text, encoding="utf-8")

        manifest = apply_manifest_values(
            manifest_template,
            script_name=script_name,
            version=args.version,
            hook=args.hook,
            stage=args.stage,
            priority=args.priority,
            exclusive_group=exclusive_group,
            target_profiles=target_profiles,
            description=args.description,
            owner=args.owner,
        )
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )

        print(f"[new_script] generated {script_path}")
        print(f"[new_script] generated {manifest_path}")
        return 0
    except Exception as exc:
        print(f"[new_script] error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
