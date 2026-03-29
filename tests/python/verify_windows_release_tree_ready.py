from __future__ import annotations

import argparse
import re
from pathlib import Path


TESTS_SUBDIR = "tests"
INCLUDE_FILE_SUFFIX = "_include.cmake"
DISCOVERY_FILE_SUFFIX = "_tests.cmake"
CONFIG_IF_PATTERN = re.compile(r'^if\(CTEST_CONFIGURATION_TYPE MATCHES "([^"]+)"\)$')
QUOTED_ADD_TEST_PATTERN = re.compile(r'add_test\(\[=\[(?P<name>.*?)\]=\]\s+"(?P<command>[^"]+)"')
EXISTS_PATTERN = re.compile(r'^if\(EXISTS "([^"]+)"\)$')


def _tests_dir(build_dir: Path) -> Path:
    return build_dir / TESTS_SUBDIR


def _collect_discovery_failures(tests_dir: Path) -> list[str]:
    failures: list[str] = []
    for include_path in sorted(tests_dir.glob(f"*{INCLUDE_FILE_SUFFIX}")):
        referenced_path: Path | None = None
        for raw_line in include_path.read_text(encoding="utf-8").splitlines():
            match = EXISTS_PATTERN.match(raw_line.strip())
            if match is None:
                continue
            referenced_path = Path(match.group(1))
            break
        if referenced_path is None or referenced_path.suffix != ".cmake":
            continue
        if referenced_path.name.endswith(DISCOVERY_FILE_SUFFIX) and not referenced_path.exists():
            failures.append(
                f"missing discovered gtest metadata: {referenced_path} "
                f"(referenced by {include_path})"
            )
    return failures


def _parse_active_direct_executable_tests(ctest_file: Path, build_dir: Path, config: str) -> list[str]:
    failures: list[str] = []
    state_stack: list[bool] = []

    for raw_line in ctest_file.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        match = CONFIG_IF_PATTERN.match(line)
        if match is not None:
            state_stack.append(re.fullmatch(match.group(1), config) is not None)
            continue
        if line == "else()":
            if state_stack:
                state_stack[-1] = not state_stack[-1]
            continue
        if line == "endif()":
            if state_stack:
                state_stack.pop()
            continue
        if state_stack and not all(state_stack):
            continue

        add_test_match = QUOTED_ADD_TEST_PATTERN.search(line)
        if add_test_match is None:
            continue

        command_path = Path(add_test_match.group("command"))
        if command_path.suffix.lower() != ".exe":
            continue
        if build_dir not in command_path.parents:
            continue
        if command_path.exists():
            continue

        failures.append(
            f"missing configured {config} executable for {add_test_match.group('name')}: {command_path}"
        )

    return failures


def collect_release_tree_failures(build_dir: Path, config: str) -> list[str]:
    tests_dir = _tests_dir(build_dir)
    if not tests_dir.exists():
        return [f"tests metadata directory is missing: {tests_dir}"]

    failures = _collect_discovery_failures(tests_dir)
    for ctest_file in sorted(tests_dir.rglob("CTestTestfile.cmake")):
        failures.extend(_parse_active_direct_executable_tests(ctest_file, build_dir, config))
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Verify that the generated Windows validation tree contains discovered gtest metadata "
            "and all directly registered Release executables required by ctest."
        )
    )
    parser.add_argument("--build-dir", default="build-windows-release")
    parser.add_argument("--config", default="Release")
    args = parser.parse_args(argv)

    build_dir = Path(args.build_dir).resolve()
    failures = collect_release_tree_failures(build_dir, args.config)
    if failures:
        print(f"FAIL windows release tree readiness: {len(failures)} issue(s) detected")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print(
        "PASS windows release tree readiness: generated metadata and directly registered "
        f"{args.config} executables are present"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
