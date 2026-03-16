#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path


INLINE_LINK_RE = re.compile(r'!?\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)')
SCHEMES = ("http://", "https://", "mailto:", "file://", "app://", "plugin://")
DEFAULT_PATTERNS = (
    "README.md",
    "docs/**/*.md",
    "tools/**/README.md",
    "core/README.md",
    "server/README.md",
    "gateway/README.md",
    "client_gui/README.md",
    "docker/stack/README.md",
)


def iter_markdown_files(repo_root: Path) -> list[Path]:
    seen: set[Path] = set()
    files: list[Path] = []
    for pattern in DEFAULT_PATTERNS:
        for path in repo_root.glob(pattern):
            if path.is_file() and path not in seen:
                seen.add(path)
                files.append(path)
    return sorted(files)


def strip_fenced_code(text: str) -> str:
    lines = []
    in_fence = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            lines.append(line)
    return "\n".join(lines)


def check_file(repo_root: Path, path: Path) -> list[str]:
    rel = path.relative_to(repo_root).as_posix()
    text = strip_fenced_code(path.read_text(encoding="utf-8", errors="replace"))
    errors: list[str] = []

    for target in INLINE_LINK_RE.findall(text):
        if target.startswith(SCHEMES):
            continue
        if target.startswith("#"):
            continue
        if target.startswith("<") and target.endswith(">"):
            target = target[1:-1]
        link_path = target.split("#", 1)[0]
        if not link_path:
            continue
        resolved = (path.parent / link_path).resolve()
        if not resolved.exists():
            errors.append(f"{rel} -> {target}")
    return errors


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    files = iter_markdown_files(repo_root)
    errors: list[str] = []
    for file_path in files:
        errors.extend(check_file(repo_root, file_path))

    if errors:
        print(f"[markdown-links] FAIL: {len(errors)} broken relative link(s)")
        for error in errors:
            print(f"- {error}")
        return 1

    print(f"[markdown-links] OK: checked {len(files)} markdown file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
