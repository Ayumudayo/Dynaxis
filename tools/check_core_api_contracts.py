#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys


INCLUDE_RE = re.compile(r"#include\s*[<\"]([^\">]+)[\">]")

STABLE_HEADERS = {
    "server/core/api/version.hpp",
    "server/core/app/app_host.hpp",
    "server/core/app/termination_signals.hpp",
    "server/core/build_info.hpp",
    "server/core/compression/compressor.hpp",
    "server/core/concurrent/job_queue.hpp",
    "server/core/concurrent/task_scheduler.hpp",
    "server/core/concurrent/thread_manager.hpp",
    "server/core/config/options.hpp",
    "server/core/metrics/build_info.hpp",
    "server/core/metrics/metrics.hpp",
    "server/core/metrics/http_server.hpp",
    "server/core/memory/memory_pool.hpp",
    "server/core/net/connection.hpp",
    "server/core/net/dispatcher.hpp",
    "server/core/net/hive.hpp",
    "server/core/net/listener.hpp",
    "server/core/protocol/packet.hpp",
    "server/core/protocol/protocol_errors.hpp",
    "server/core/protocol/protocol_flags.hpp",
    "server/core/protocol/system_opcodes.hpp",
    "server/core/runtime_metrics.hpp",
    "server/core/security/cipher.hpp",
    "server/core/util/log.hpp",
    "server/core/util/paths.hpp",
    "server/core/util/service_registry.hpp",
}

INTERNAL_HEADERS = {
    "server/core/concurrent/locked_queue.hpp",
    "server/core/net/acceptor.hpp",
    "server/core/net/connection_runtime_state.hpp",
    "server/core/net/session.hpp",
    "server/core/state/instance_registry.hpp",
    "server/core/storage/connection_pool.hpp",
    "server/core/storage/db_worker_pool.hpp",
    "server/core/storage/redis/client.hpp",
    "server/core/storage/unit_of_work.hpp",
    "server/core/util/crash_handler.hpp",
}

PR_REQUIRED_FIELDS = (
    "API boundary classification touched",
    "Stable header changed",
    "Breaking change on Stable API",
    "Migration note path (required if breaking=yes)",
    "Compatibility matrix updated (docs/core-api/compatibility-matrix.json)",
    "Public API version updated (core/include/server/core/api/version.hpp)",
    "Docs updated (docs/core-api/** or docs/core-api-boundary.md)",
    "Public API smoke check command/result",
)

API_VERSION_HEADER_REPO_PATH = "core/include/server/core/api/version.hpp"
COMPATIBILITY_MATRIX_REPO_PATH = "docs/core-api/compatibility-matrix.json"
STABLE_GOVERNANCE_FIXTURES_REPO_PATH = (
    "tests/core/fixtures/api_contracts/stable_governance_cases.json"
)


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def normalize_repo_path(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    return path.resolve().relative_to(repo_root.resolve()).as_posix()


def iter_core_api_docs(repo_root: pathlib.Path) -> list[pathlib.Path]:
    docs_dir = repo_root / "docs" / "core-api"
    if not docs_dir.exists():
        return []
    return sorted(docs_dir.rglob("*.md"))


def iter_public_example_tests(repo_root: pathlib.Path) -> list[pathlib.Path]:
    tests_dir = repo_root / "tests" / "core"
    if not tests_dir.exists():
        return []
    return sorted(tests_dir.glob("public_api*.cpp"))


def iter_boundary_fixture_tests(
    repo_root: pathlib.Path, prefix: str
) -> list[pathlib.Path]:
    fixtures_dir = repo_root / "tests" / "core" / "fixtures" / "api_contracts"
    if not fixtures_dir.exists():
        return []
    return sorted(fixtures_dir.glob(f"{prefix}*.cpp"))


def iter_core_public_headers(repo_root: pathlib.Path) -> list[pathlib.Path]:
    headers_dir = repo_root / "core" / "include" / "server" / "core"
    if not headers_dir.exists():
        return []
    return sorted(headers_dir.rglob("*.hpp"))


def stable_header_repo_paths() -> set[str]:
    return {f"core/include/{header}" for header in STABLE_HEADERS}


def find_includes(text: str) -> list[str]:
    return [match.group(1).strip() for match in INCLUDE_RE.finditer(text)]


def classify_core_public_include(include_path: str) -> str | None:
    if not include_path.startswith("server/core/"):
        return None
    if include_path in INTERNAL_HEADERS:
        return "internal"
    if include_path not in STABLE_HEADERS:
        return "non_stable"
    return "stable"


def check_boundary_contract(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []

    smoke_file = repo_root / "tests" / "core" / "public_api_smoke.cpp"
    if not smoke_file.exists():
        errors.append("missing required smoke file: tests/core/public_api_smoke.cpp")
        return errors

    smoke_text = read_text(smoke_file)
    smoke_includes = [
        inc for inc in find_includes(smoke_text) if inc.startswith("server/core/")
    ]
    smoke_include_set = set(smoke_includes)

    for inc in smoke_includes:
        if inc in INTERNAL_HEADERS:
            errors.append(f"smoke file includes internal header: {inc}")
        if inc not in STABLE_HEADERS:
            errors.append(f"smoke file includes non-stable header: {inc}")

    for stable in sorted(STABLE_HEADERS - smoke_include_set):
        errors.append(f"smoke file missing Stable header include: {stable}")

    for doc_file in iter_core_api_docs(repo_root):
        text = read_text(doc_file)
        doc_rel = normalize_repo_path(doc_file, repo_root)

        for inc in find_includes(text):
            if not inc.startswith("server/core/"):
                continue
            if inc in INTERNAL_HEADERS:
                errors.append(f"{doc_rel} references internal header include: {inc}")

        for internal in INTERNAL_HEADERS:
            if internal in text:
                errors.append(f"{doc_rel} references internal header path: {internal}")

    public_docs = [repo_root / "README.md", repo_root / "core" / "README.md"]
    for doc_file in public_docs:
        if not doc_file.exists():
            continue
        text = read_text(doc_file)
        doc_rel = normalize_repo_path(doc_file, repo_root)
        for inc in find_includes(text):
            if inc in INTERNAL_HEADERS:
                errors.append(f"{doc_rel} references internal header include: {inc}")

    for test_file in iter_public_example_tests(repo_root):
        text = read_text(test_file)
        test_rel = normalize_repo_path(test_file, repo_root)
        for inc in find_includes(text):
            if not inc.startswith("server/core/"):
                continue
            if inc in INTERNAL_HEADERS:
                errors.append(f"{test_rel} references internal header include: {inc}")
            if inc not in STABLE_HEADERS:
                errors.append(f"{test_rel} references non-Stable header include: {inc}")

    errors.extend(check_public_header_dependency_hygiene(repo_root))

    errors.extend(check_compatibility_matrix(repo_root))

    return errors


def check_boundary_fixtures(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []

    good_fixtures = iter_boundary_fixture_tests(repo_root, "good_")
    bad_fixtures = iter_boundary_fixture_tests(repo_root, "bad_")

    if not good_fixtures:
        errors.append(
            "missing good boundary fixture: tests/core/fixtures/api_contracts/good_*.cpp"
        )
    if not bad_fixtures:
        errors.append(
            "missing bad boundary fixture: tests/core/fixtures/api_contracts/bad_*.cpp"
        )

    for fixture in good_fixtures:
        fixture_rel = normalize_repo_path(fixture, repo_root)
        for include_path in find_includes(read_text(fixture)):
            category = classify_core_public_include(include_path)
            if category == "internal":
                errors.append(
                    f"{fixture_rel} (good fixture) includes internal header: {include_path}"
                )
            elif category == "non_stable":
                errors.append(
                    f"{fixture_rel} (good fixture) includes non-Stable header: {include_path}"
                )

    for fixture in bad_fixtures:
        fixture_rel = normalize_repo_path(fixture, repo_root)
        has_expected_violation = False
        for include_path in find_includes(read_text(fixture)):
            category = classify_core_public_include(include_path)
            if category in {"internal", "non_stable"}:
                has_expected_violation = True
                break
        if not has_expected_violation:
            errors.append(
                f"{fixture_rel} (bad fixture) must include at least one internal or non-Stable core header"
            )

    return errors


def check_public_header_dependency_hygiene(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []

    for header_file in iter_core_public_headers(repo_root):
        header_rel = normalize_repo_path(header_file, repo_root)
        text = read_text(header_file)
        for inc in find_includes(text):
            if inc.startswith("gateway/"):
                errors.append(f"{header_rel} must not include gateway header: {inc}")

            if (
                inc.startswith("server/")
                and not inc.startswith("server/core/")
                and not inc.startswith("server/wire/")
            ):
                errors.append(
                    f"{header_rel} must not include non-core server header: {inc}"
                )

    return errors


def parse_api_version(repo_root: pathlib.Path) -> tuple[str, list[str]]:
    errors: list[str] = []
    version_header = repo_root / API_VERSION_HEADER_REPO_PATH
    if not version_header.exists():
        errors.append(f"missing API version header: {API_VERSION_HEADER_REPO_PATH}")
        return "", errors

    text = read_text(version_header)

    major_match = re.search(r"k_version_major\s*=\s*(\d+)", text)
    minor_match = re.search(r"k_version_minor\s*=\s*(\d+)", text)
    patch_match = re.search(r"k_version_patch\s*=\s*(\d+)", text)
    literal_match = re.search(
        r'version_string\s*\(\)\s*noexcept\s*\{\s*return\s*"([^"]+)"', text
    )

    if not major_match or not minor_match or not patch_match:
        errors.append(
            f"failed to parse API version tuple from: {API_VERSION_HEADER_REPO_PATH}"
        )
        return "", errors

    version_tuple = (
        f"{major_match.group(1)}.{minor_match.group(1)}.{patch_match.group(1)}"
    )
    if not literal_match:
        errors.append(
            f"failed to parse version_string() from: {API_VERSION_HEADER_REPO_PATH}"
        )
        return version_tuple, errors

    version_literal = literal_match.group(1)
    if version_literal != version_tuple:
        errors.append(
            "version tuple and version_string() mismatch in "
            f"{API_VERSION_HEADER_REPO_PATH}: tuple={version_tuple}, literal={version_literal}"
        )

    return version_tuple, errors


def check_compatibility_matrix(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    matrix_path = repo_root / COMPATIBILITY_MATRIX_REPO_PATH
    if not matrix_path.exists():
        errors.append(f"missing compatibility matrix: {COMPATIBILITY_MATRIX_REPO_PATH}")
        return errors

    try:
        matrix = json.loads(read_text(matrix_path))
    except json.JSONDecodeError as exc:
        errors.append(f"failed to parse compatibility matrix JSON: {exc}")
        return errors

    if not isinstance(matrix, dict):
        errors.append("compatibility matrix root must be an object")
        return errors

    matrix_version = matrix.get("api_version")
    if not isinstance(matrix_version, str) or not matrix_version.strip():
        errors.append("compatibility matrix must include non-empty string api_version")

    stable_entries = matrix.get("stable_headers")
    if not isinstance(stable_entries, list):
        errors.append("compatibility matrix must include stable_headers array")
        return errors

    matrix_paths: set[str] = set()
    for entry in stable_entries:
        if not isinstance(entry, dict):
            errors.append("stable_headers entries must be objects")
            continue
        path = entry.get("path")
        if not isinstance(path, str) or not path:
            errors.append("stable_headers entry missing path")
            continue
        matrix_paths.add(path)

    missing = sorted(STABLE_HEADERS - matrix_paths)
    extra = sorted(matrix_paths - STABLE_HEADERS)
    for path in missing:
        errors.append(f"compatibility matrix missing Stable header: {path}")
    for path in extra:
        errors.append(f"compatibility matrix contains non-Stable header: {path}")

    parsed_version, parse_errors = parse_api_version(repo_root)
    errors.extend(parse_errors)
    if (
        parsed_version
        and isinstance(matrix_version, str)
        and matrix_version != parsed_version
    ):
        errors.append(
            "compatibility matrix api_version does not match "
            f"{API_VERSION_HEADER_REPO_PATH}: matrix={matrix_version}, header={parsed_version}"
        )

    return errors


def git_changed_files(
    repo_root: pathlib.Path, base_sha: str, head_sha: str
) -> list[str]:
    command = ["git", "diff", "--name-only", base_sha, head_sha]
    result = subprocess.run(command, cwd=repo_root, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git diff failed")
    return [
        line.strip().replace("\\", "/")
        for line in result.stdout.splitlines()
        if line.strip()
    ]


def parse_pr_field(body: str, field_name: str) -> str | None:
    pattern = re.compile(
        rf"^\s*-\s*{re.escape(field_name)}\s*:\s*(.*?)\s*$", re.MULTILINE
    )
    match = pattern.search(body)
    if not match:
        return None
    return match.group(1).strip()


def is_placeholder_value(value: str) -> bool:
    normalized = value.strip().lower()
    if not normalized:
        return True

    placeholders = {
        "required",
        "todo",
        "tbd",
        "fill",
        "fill me",
        "<required>",
        "[required]",
        "<fill>",
        "<fill me>",
    }
    return normalized in placeholders


def parse_pr_body_from_event(event_path: pathlib.Path) -> str:
    raw = read_text(event_path)
    payload = json.loads(raw)
    pull_request = payload.get("pull_request")
    if not isinstance(pull_request, dict):
        return ""
    body = pull_request.get("body")
    if not isinstance(body, str):
        return ""
    return body


def check_pr_governance(
    repo_root: pathlib.Path,
    base_sha: str,
    head_sha: str,
    event_path: str,
) -> list[str]:
    errors: list[str] = []
    changed = git_changed_files(repo_root, base_sha, head_sha)
    api_changed = any(path.startswith("core/include/server/core/") for path in changed)
    if not api_changed:
        return errors

    if not event_path:
        errors.append("--check-pr-governance requires --event-path")
        return errors

    payload_path = pathlib.Path(event_path)
    if not payload_path.exists():
        errors.append(f"pull_request event payload not found: {event_path}")
        return errors

    try:
        body = parse_pr_body_from_event(payload_path)
    except json.JSONDecodeError as exc:
        errors.append(f"failed to parse event payload JSON: {exc}")
        return errors

    if not body.strip():
        errors.append(
            "API-touching PR requires Core API Impact section in PR body "
            "(body is empty or missing)"
        )
        return errors

    field_values: dict[str, str] = {}
    for field_name in PR_REQUIRED_FIELDS:
        value = parse_pr_field(body, field_name)
        if value is None:
            errors.append(f"missing PR governance field: {field_name}")
            continue
        if is_placeholder_value(value):
            errors.append(f"PR governance field is not filled: {field_name}")
            continue
        field_values[field_name] = value

    breaking = field_values.get("Breaking change on Stable API", "").strip().lower()
    if breaking in {"yes", "y", "true", "1"}:
        migration = field_values.get(
            "Migration note path (required if breaking=yes)", ""
        ).strip()
        migration_lower = migration.lower()
        if migration_lower in {"n/a", "na", "none", "no"} or not migration.startswith(
            "docs/core-api/"
        ):
            errors.append(
                "breaking Stable API changes require a migration note path under docs/core-api/"
            )

    return errors


def check_doc_freshness(
    repo_root: pathlib.Path, base_sha: str, head_sha: str
) -> list[str]:
    errors: list[str] = []
    changed = git_changed_files(repo_root, base_sha, head_sha)

    api_changed = any(path.startswith("core/include/server/core/") for path in changed)
    if not api_changed:
        return errors

    docs_changed = any(
        path.startswith("docs/core-api/") or path == "docs/core-api-boundary.md"
        for path in changed
    )
    if not docs_changed:
        errors.append(
            "core public headers changed without core API docs update "
            "(expected change under docs/core-api/ or docs/core-api-boundary.md)"
        )

    return errors


def check_stable_change_governance(
    repo_root: pathlib.Path, base_sha: str, head_sha: str
) -> list[str]:
    changed = set(git_changed_files(repo_root, base_sha, head_sha))
    return check_stable_change_governance_from_changed_files(changed)


def check_stable_change_governance_from_changed_files(changed: set[str]) -> list[str]:
    errors: list[str] = []

    stable_repo_headers = stable_header_repo_paths()
    changed_stable_headers = sorted(
        path
        for path in changed
        if path in stable_repo_headers and path != API_VERSION_HEADER_REPO_PATH
    )
    if not changed_stable_headers:
        return errors

    if API_VERSION_HEADER_REPO_PATH not in changed:
        errors.append(
            "Stable header changed without API version update "
            f"(expected change in {API_VERSION_HEADER_REPO_PATH})"
        )

    if COMPATIBILITY_MATRIX_REPO_PATH not in changed:
        errors.append(
            "Stable header changed without compatibility matrix update "
            f"(expected change in {COMPATIBILITY_MATRIX_REPO_PATH})"
        )

    return errors


def check_stable_governance_fixtures(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    fixtures_path = repo_root / STABLE_GOVERNANCE_FIXTURES_REPO_PATH
    if not fixtures_path.exists():
        errors.append(
            f"missing stable governance fixture file: {STABLE_GOVERNANCE_FIXTURES_REPO_PATH}"
        )
        return errors

    try:
        payload = json.loads(read_text(fixtures_path))
    except json.JSONDecodeError as exc:
        errors.append(f"failed to parse stable governance fixture JSON: {exc}")
        return errors

    cases = payload.get("cases") if isinstance(payload, dict) else None
    if not isinstance(cases, list) or not cases:
        errors.append("stable governance fixtures must include non-empty 'cases' array")
        return errors

    for index, case in enumerate(cases, start=1):
        case_label = f"case#{index}"
        if not isinstance(case, dict):
            errors.append(f"{case_label}: fixture entry must be an object")
            continue

        name = case.get("name")
        if isinstance(name, str) and name.strip():
            case_label = name.strip()

        changed = case.get("changed")
        expected = case.get("expected_error_substrings")

        if not isinstance(changed, list) or not all(
            isinstance(item, str) and item for item in changed
        ):
            errors.append(
                f"{case_label}: 'changed' must be an array of non-empty strings"
            )
            continue

        if not isinstance(expected, list) or not all(
            isinstance(item, str) and item for item in expected
        ):
            errors.append(
                f"{case_label}: 'expected_error_substrings' must be an array of non-empty strings"
            )
            continue

        actual_errors = check_stable_change_governance_from_changed_files(set(changed))
        missing_expected = [
            token
            for token in expected
            if not any(token in err for err in actual_errors)
        ]
        unexpected_actual = [
            err for err in actual_errors if not any(token in err for token in expected)
        ]

        if missing_expected:
            errors.append(
                f"{case_label}: missing expected stable-governance error(s): {', '.join(missing_expected)}"
            )
        if unexpected_actual:
            errors.append(
                f"{case_label}: unexpected stable-governance error(s): {' | '.join(unexpected_actual)}"
            )

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check server_core API boundary and docs governance rules"
    )
    parser.add_argument(
        "--check-boundary",
        action="store_true",
        help="validate public/internal include usage",
    )
    parser.add_argument(
        "--check-boundary-fixtures",
        action="store_true",
        help="validate boundary checker fixtures for positive/negative include cases",
    )
    parser.add_argument(
        "--check-doc-freshness",
        action="store_true",
        help="require docs update when API headers change",
    )
    parser.add_argument(
        "--check-pr-governance",
        action="store_true",
        help="validate PR governance fields for API-touching PRs",
    )
    parser.add_argument(
        "--check-stable-change-governance",
        action="store_true",
        help="require version and matrix updates for Stable header changes",
    )
    parser.add_argument(
        "--check-stable-governance-fixtures",
        action="store_true",
        help="validate stable governance regression fixture cases",
    )
    parser.add_argument(
        "--base-sha", default="", help="base commit SHA for docs freshness check"
    )
    parser.add_argument(
        "--head-sha", default="", help="head commit SHA for docs freshness check"
    )
    parser.add_argument(
        "--event-path", default="", help="GitHub event payload path for PR body checks"
    )
    args = parser.parse_args()

    if (
        not args.check_boundary
        and not args.check_boundary_fixtures
        and not args.check_doc_freshness
        and not args.check_pr_governance
        and not args.check_stable_change_governance
        and not args.check_stable_governance_fixtures
    ):
        args.check_boundary = True

    repo_root = pathlib.Path(__file__).resolve().parents[1]
    all_errors: list[str] = []

    if args.check_boundary:
        all_errors.extend(check_boundary_contract(repo_root))

    if args.check_boundary_fixtures:
        all_errors.extend(check_boundary_fixtures(repo_root))

    if args.check_doc_freshness:
        if not args.base_sha or not args.head_sha:
            all_errors.append(
                "--check-doc-freshness requires --base-sha and --head-sha"
            )
        else:
            try:
                all_errors.extend(
                    check_doc_freshness(repo_root, args.base_sha, args.head_sha)
                )
            except RuntimeError as exc:
                all_errors.append(str(exc))

    if args.check_pr_governance:
        if not args.base_sha or not args.head_sha:
            all_errors.append(
                "--check-pr-governance requires --base-sha and --head-sha"
            )
        else:
            try:
                all_errors.extend(
                    check_pr_governance(
                        repo_root,
                        args.base_sha,
                        args.head_sha,
                        args.event_path,
                    )
                )
            except RuntimeError as exc:
                all_errors.append(str(exc))

    if args.check_stable_change_governance:
        if not args.base_sha or not args.head_sha:
            all_errors.append(
                "--check-stable-change-governance requires --base-sha and --head-sha"
            )
        else:
            try:
                all_errors.extend(
                    check_stable_change_governance(
                        repo_root, args.base_sha, args.head_sha
                    )
                )
            except RuntimeError as exc:
                all_errors.append(str(exc))

    if args.check_stable_governance_fixtures:
        all_errors.extend(check_stable_governance_fixtures(repo_root))

    if all_errors:
        for err in all_errors:
            print(f"[core-api-check] ERROR: {err}")
        return 1

    print("[core-api-check] OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
