#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


HOOK_CHOICES = (
    "on_chat_send",
    "on_login",
    "on_join",
    "on_leave",
    "on_session_event",
    "on_admin_command",
)
OPTIONAL_HOOKS = (
    "on_login",
    "on_join",
    "on_leave",
    "on_session_event",
    "on_admin_command",
)
STAGE_CHOICES = ("pre_validate", "mutate", "authorize", "side_effect", "observe")
TARGET_PROFILE_CHOICES = ("chat", "world", "all")

CPP_TEMPLATE_REL = Path("server/plugins/templates/chat_hook_v2_template.cpp")
MANIFEST_TEMPLATE_REL = Path("server/plugins/templates/plugin_manifest.template.json")

POINTER_MARKERS = {
    "on_login": "/* __ON_LOGIN_PTR__ */ nullptr",
    "on_join": "/* __ON_JOIN_PTR__ */ nullptr",
    "on_leave": "/* __ON_LEAVE_PTR__ */ nullptr",
    "on_session_event": "/* __ON_SESSION_EVENT_PTR__ */ nullptr",
    "on_admin_command": "/* __ON_ADMIN_COMMAND_PTR__ */ nullptr",
}

HOOK_STUBS = {
    "on_login": """HookDecisionV2 CHAT_HOOK_CALL on_login(
    void*,
    const LoginEventV2* in,
    LoginEventOutV2* out) {
    (void)in;
    (void)out;
    return HookDecisionV2::kPass;
}""",
    "on_join": """HookDecisionV2 CHAT_HOOK_CALL on_join(
    void*,
    const JoinEventV2* in,
    JoinEventOutV2* out) {
    (void)in;
    (void)out;
    return HookDecisionV2::kPass;
}""",
    "on_leave": """HookDecisionV2 CHAT_HOOK_CALL on_leave(
    void*,
    const LeaveEventV2* in) {
    (void)in;
    return HookDecisionV2::kPass;
}""",
    "on_session_event": """HookDecisionV2 CHAT_HOOK_CALL on_session_event(
    void*,
    const SessionEventV2* in) {
    (void)in;
    return HookDecisionV2::kPass;
}""",
    "on_admin_command": """HookDecisionV2 CHAT_HOOK_CALL on_admin_command(
    void*,
    const AdminCommandV2* in,
    AdminCommandOutV2* out) {
    (void)in;
    (void)out;
    return HookDecisionV2::kPass;
}""",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def sanitize_name(raw: str) -> str:
    token = re.sub(r"[^A-Za-z0-9_]+", "_", raw.strip())
    token = token.strip("_").lower()
    if not token:
        raise ValueError("--name must contain at least one alphanumeric character")
    if token[0].isdigit():
        token = f"p_{token}"
    return token


def normalize_hooks(raw_hooks: list[str] | None) -> list[str]:
    hooks = list(raw_hooks or ["on_chat_send"])
    deduped: list[str] = []
    seen: set[str] = set()
    for hook in hooks:
        if hook in seen:
            continue
        deduped.append(hook)
        seen.add(hook)
    if "on_chat_send" not in seen:
        deduped.insert(0, "on_chat_send")
    return deduped


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


def render_cpp(
    template_text: str, plugin_name: str, version: str, hooks: list[str]
) -> str:
    selected = set(hooks)
    hook_stubs: list[str] = []
    for hook in OPTIONAL_HOOKS:
        if hook in selected:
            hook_stubs.append(HOOK_STUBS[hook])

    rendered = template_text
    rendered = rendered.replace("__PLUGIN_NAME__", plugin_name)
    rendered = rendered.replace("__PLUGIN_VERSION__", version)

    stub_block = "\n\n".join(hook_stubs)
    rendered = rendered.replace("// __HOOK_STUBS__", stub_block)

    for hook in OPTIONAL_HOOKS:
        marker = POINTER_MARKERS[hook]
        pointer = f"&{hook}" if hook in selected else "nullptr"
        rendered = rendered.replace(marker, pointer)

    if not rendered.endswith("\n"):
        rendered += "\n"
    return rendered


def load_manifest_template(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def apply_manifest_values(
    manifest: dict[str, object],
    *,
    plugin_name: str,
    version: str,
    hooks: list[str],
    stage: str,
    priority: int,
    exclusive_group: str,
    target_profiles: list[str],
    description: str | None,
    owner: str | None,
) -> dict[str, object]:
    manifest["name"] = plugin_name
    manifest["version"] = version
    manifest["hook_scope"] = hooks
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
        description="Create a ChatHook ABI v2 plugin scaffold."
    )
    parser.add_argument(
        "--name",
        required=True,
        help="Plugin name (used for file names and manifest name)",
    )
    parser.add_argument(
        "--hook",
        action="append",
        choices=HOOK_CHOICES,
        help="Hook to implement (repeatable)",
    )
    parser.add_argument("--version", default="0.1.0", help="Plugin version string")
    parser.add_argument(
        "--stage", choices=STAGE_CHOICES, default="mutate", help="Manifest stage value"
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
        default="server/plugins",
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
    cpp_template_path = root / CPP_TEMPLATE_REL
    manifest_template_path = root / MANIFEST_TEMPLATE_REL
    output_dir = resolve_output_dir(root, args.output_dir)

    plugin_name = sanitize_name(args.name)
    hooks = normalize_hooks(args.hook)
    profiles = normalize_profiles(args.target_profile)
    exclusive_group = args.exclusive_group or plugin_name

    prefix = f"{args.priority:02d}" if args.priority < 100 else str(args.priority)
    stem = f"{prefix}_{plugin_name}"
    source_path = output_dir / f"{stem}.cpp"
    manifest_path = output_dir / f"{stem}.plugin.json"

    try:
        template_text = cpp_template_path.read_text(encoding="utf-8")
        manifest_template = load_manifest_template(manifest_template_path)

        output_dir.mkdir(parents=True, exist_ok=True)
        ensure_writable(source_path, args.force)
        ensure_writable(manifest_path, args.force)

        source_text = render_cpp(template_text, plugin_name, args.version, hooks)
        source_path.write_text(source_text, encoding="utf-8")

        manifest = apply_manifest_values(
            manifest_template,
            plugin_name=plugin_name,
            version=args.version,
            hooks=hooks,
            stage=args.stage,
            priority=args.priority,
            exclusive_group=exclusive_group,
            target_profiles=profiles,
            description=args.description,
            owner=args.owner,
        )
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )

        print(f"[new_plugin] generated {source_path}")
        print(f"[new_plugin] generated {manifest_path}")
        print(
            "[new_plugin] next: add the generated source file to server/plugins/CMakeLists.txt"
        )
        return 0
    except Exception as exc:
        print(f"[new_plugin] error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
